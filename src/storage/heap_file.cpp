#include "heap_file.h"

#include "../txn/transaction.h"

#include <algorithm>
#include <stdexcept>

namespace flintdb {

namespace {

// Acquires `mode` on (object_id, page_id) through the calling thread's
// current transaction, if any -- a no-op when GetCurrentTransaction() is
// nullptr, preserving every pre-Phase-4, non-transactional caller's
// behavior exactly. Mirrors btree.cpp's identical helper. Qualified by
// ObjectId as of Phase 5 (docs/DECISIONS.md D-032), since page_id alone
// is only unique within this HeapFile's own file.
void LockPage(ObjectId object_id, PageId page_id, LockMode mode) {
    if (Transaction* txn = GetCurrentTransaction()) txn->AcquireLock(object_id, page_id, mode);
}

}  // namespace

HeapFile::HeapFile(ObjectId object_id, BufferPool* buffer_pool) : object_id_(object_id), buffer_pool_(buffer_pool) {
    // No locking needed: object construction happens before any other
    // thread can possibly have a reference to this HeapFile.
    size_t existing = buffer_pool_->NumPagesOnDisk();
    page_ids_.reserve(existing);
    for (PageId pid = 0; pid < existing; pid++) {
        page_ids_.push_back(pid);
    }
}

RID HeapFile::Insert(const std::string& row_bytes) {
    if (row_bytes.size() > kMaxRowSize) {
        throw std::invalid_argument("HeapFile::Insert: row is larger than a page can ever hold");
    }

    // Common case: the most recently allocated page still has room. The
    // page_ids_ read is a quick snapshot under the latch, released
    // before locking/fetching the page itself -- see the class comment
    // on why a page that's no longer truly "last" by the time we act on
    // it is fine (InsertRecord's own nullopt fallback already handles
    // that, same as before Phase 4).
    bool have_last;
    PageId last = INVALID_PAGE_ID;
    {
        std::lock_guard<std::mutex> lock(page_ids_mutex_);
        have_last = !page_ids_.empty();
        if (have_last) last = page_ids_.back();
    }
    if (have_last) {
        LockPage(object_id_, last, LockMode::kExclusive);
        Page* page = buffer_pool_->FetchPage(last);
        auto slot = page->InsertRecord(row_bytes);
        if (slot.has_value()) {
            buffer_pool_->MarkDirty(last);
            return RID{last, *slot};
        }
    }

    // Otherwise allocate a fresh page for it.
    PageId new_pid;
    Page* page = buffer_pool_->NewHeapPage(&new_pid);
    LockPage(object_id_, new_pid, LockMode::kExclusive);  // bookkeeping only -- a brand-new page can't conflict with anyone
    {
        std::lock_guard<std::mutex> lock(page_ids_mutex_);
        page_ids_.push_back(new_pid);
    }
    auto slot = page->InsertRecord(row_bytes);
    if (!slot.has_value()) {
        // Can only happen if kMaxRowSize above is wrong; a fresh empty
        // page has PAGE_SIZE - kHeaderSize bytes free by construction.
        throw std::logic_error("HeapFile::Insert: row did not fit in a brand-new empty page");
    }
    buffer_pool_->MarkDirty(new_pid);
    return RID{new_pid, *slot};
}

std::optional<std::string> HeapFile::GetRow(RID rid) const {
    bool owns_page;
    {
        std::lock_guard<std::mutex> lock(page_ids_mutex_);
        owns_page = std::find(page_ids_.begin(), page_ids_.end(), rid.page_id) != page_ids_.end();
    }
    if (!owns_page) {
        return std::nullopt;  // not a page this heap file owns
    }
    LockPage(object_id_, rid.page_id, LockMode::kShared);
    Page* page = buffer_pool_->FetchPage(rid.page_id);
    if (rid.slot_id >= page->GetSlotCount() || page->IsDeleted(rid.slot_id)) {
        return std::nullopt;
    }
    return page->GetRecord(rid.slot_id);
}

std::vector<std::pair<RID, std::string>> HeapFile::Scan() const {
    std::vector<PageId> page_ids_snapshot;
    {
        std::lock_guard<std::mutex> lock(page_ids_mutex_);
        page_ids_snapshot = page_ids_;
    }

    std::vector<std::pair<RID, std::string>> out;
    for (PageId pid : page_ids_snapshot) {
        LockPage(object_id_, pid, LockMode::kShared);
        Page* page = buffer_pool_->FetchPage(pid);
        uint16_t n = page->GetSlotCount();
        for (SlotId s = 0; s < n; s++) {
            if (page->IsDeleted(s)) continue;
            out.emplace_back(RID{pid, s}, page->GetRecord(s));
        }
    }
    return out;
}

size_t HeapFile::NumRows() const { return Scan().size(); }

std::optional<RID> HeapFile::Find(const std::function<bool(const std::string&)>& pred) const {
    std::vector<PageId> page_ids_snapshot;
    {
        std::lock_guard<std::mutex> lock(page_ids_mutex_);
        page_ids_snapshot = page_ids_;
    }

    for (PageId pid : page_ids_snapshot) {
        LockPage(object_id_, pid, LockMode::kShared);
        Page* page = buffer_pool_->FetchPage(pid);
        uint16_t n = page->GetSlotCount();
        for (SlotId s = 0; s < n; s++) {
            if (page->IsDeleted(s)) continue;
            std::string record = page->GetRecord(s);
            if (pred(record)) return RID{pid, s};
        }
    }
    return std::nullopt;
}

bool HeapFile::Delete(RID rid) {
    bool owns_page;
    {
        std::lock_guard<std::mutex> lock(page_ids_mutex_);
        owns_page = std::find(page_ids_.begin(), page_ids_.end(), rid.page_id) != page_ids_.end();
    }
    if (!owns_page) {
        return false;  // not a page this heap file owns
    }
    LockPage(object_id_, rid.page_id, LockMode::kExclusive);
    Page* page = buffer_pool_->FetchPage(rid.page_id);
    if (rid.slot_id >= page->GetSlotCount() || page->IsDeleted(rid.slot_id)) {
        return false;
    }
    page->DeleteRecord(rid.slot_id);
    buffer_pool_->MarkDirty(rid.page_id);
    return true;
}

}  // namespace flintdb
