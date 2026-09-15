#include "heap_file.h"

#include <stdexcept>

namespace flintdb {

HeapFile::HeapFile(BufferPool* buffer_pool) : buffer_pool_(buffer_pool) {
    size_t existing = buffer_pool_->NumPagesOnDisk();
    page_ids_.reserve(existing);
    for (PageId pid = 0; pid < existing; pid++) {
        page_ids_.push_back(pid);
    }
}

RID HeapFile::Insert(const std::string& row_bytes) {
    constexpr size_t kMaxRowSize = PAGE_SIZE - Page::kHeaderSize - Page::kSlotSize;
    if (row_bytes.size() > kMaxRowSize) {
        throw std::invalid_argument("HeapFile::Insert: row is larger than a page can ever hold");
    }

    // Common case: the most recently allocated page still has room.
    if (!page_ids_.empty()) {
        PageId last = page_ids_.back();
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
    page_ids_.push_back(new_pid);
    auto slot = page->InsertRecord(row_bytes);
    if (!slot.has_value()) {
        // Can only happen if kMaxRowSize above is wrong; a fresh empty
        // page has PAGE_SIZE - kHeaderSize bytes free by construction.
        throw std::logic_error("HeapFile::Insert: row did not fit in a brand-new empty page");
    }
    buffer_pool_->MarkDirty(new_pid);
    return RID{new_pid, *slot};
}

std::vector<std::pair<RID, std::string>> HeapFile::Scan() const {
    std::vector<std::pair<RID, std::string>> out;
    for (PageId pid : page_ids_) {
        Page* page = buffer_pool_->FetchPage(pid);
        uint16_t n = page->GetSlotCount();
        for (SlotId s = 0; s < n; s++) {
            out.emplace_back(RID{pid, s}, page->GetRecord(s));
        }
    }
    return out;
}

size_t HeapFile::NumRows() const { return Scan().size(); }

std::optional<RID> HeapFile::Find(const std::function<bool(const std::string&)>& pred) const {
    for (PageId pid : page_ids_) {
        Page* page = buffer_pool_->FetchPage(pid);
        uint16_t n = page->GetSlotCount();
        for (SlotId s = 0; s < n; s++) {
            std::string record = page->GetRecord(s);
            if (pred(record)) return RID{pid, s};
        }
    }
    return std::nullopt;
}

bool HeapFile::Delete(RID rid) {
    auto rows = Scan();

    bool found = false;
    std::vector<std::string> remaining;
    remaining.reserve(rows.size());
    for (auto& [r, bytes] : rows) {
        if (!found && r == rid) {
            found = true;
            continue;
        }
        remaining.push_back(std::move(bytes));
    }
    if (!found) return false;

    buffer_pool_->ResetAll();
    page_ids_.clear();

    for (auto& bytes : remaining) {
        Insert(bytes);
    }
    return true;
}

}  // namespace flintdb
