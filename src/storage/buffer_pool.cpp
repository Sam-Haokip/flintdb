#include "buffer_pool.h"

namespace flintdb {

BufferPool::BufferPool(DiskManager* disk_manager) : disk_manager_(disk_manager) {}

Page* BufferPool::NewPage(PageId* out_page_id) {
    PageId pid = disk_manager_->AllocatePage();
    auto page = std::make_unique<Page>();  // std::array default-inits to all zero
    Page* raw = page.get();
    pages_[pid] = std::move(page);
    MarkDirty(pid);  // routes through MarkDirty (not a direct dirty_.insert) so the
                      // active-transaction observer, if any, sees new pages too
    if (out_page_id) *out_page_id = pid;
    return raw;
}

Page* BufferPool::NewHeapPage(PageId* out_page_id) {
    PageId pid;
    Page* page = NewPage(&pid);
    page->InitHeapPage(pid);
    if (out_page_id) *out_page_id = pid;
    return page;
}

Page* BufferPool::FetchPage(PageId page_id) {
    auto it = pages_.find(page_id);
    if (it != pages_.end()) return it->second.get();

    auto page = std::make_unique<Page>();
    disk_manager_->ReadPage(page_id, page->Data());
    Page* raw = page.get();
    pages_[page_id] = std::move(page);
    return raw;
}

void BufferPool::MarkDirty(PageId page_id) {
    dirty_.insert(page_id);
    if (active_txn_observer_) active_txn_observer_(page_id);
}

void BufferPool::FlushAll() {
    for (PageId pid : dirty_) {
        auto it = pages_.find(pid);
        if (it != pages_.end()) {
            disk_manager_->WritePage(pid, it->second->Data());
        }
    }
    dirty_.clear();
}

size_t BufferPool::NumPagesInMemory() const { return pages_.size(); }
size_t BufferPool::NumPagesOnDisk() const { return disk_manager_->NumPages(); }

void BufferPool::ResetAll() {
    pages_.clear();
    dirty_.clear();
    disk_manager_->ResetFile();
}

void BufferPool::SetActiveTransactionObserver(std::function<void(PageId)> observer) {
    active_txn_observer_ = std::move(observer);
}

void BufferPool::ClearActiveTransactionObserver() { active_txn_observer_ = nullptr; }

void BufferPool::DiscardPage(PageId page_id) {
    auto it = pages_.find(page_id);
    if (it == pages_.end()) return;  // never cached -- nothing to discard
    disk_manager_->ReadPage(page_id, it->second->Data());
    dirty_.erase(page_id);
}

}  // namespace flintdb
