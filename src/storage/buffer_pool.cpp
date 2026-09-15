#include "buffer_pool.h"

namespace flintdb {

BufferPool::BufferPool(DiskManager* disk_manager) : disk_manager_(disk_manager) {}

Page* BufferPool::NewPage(PageId* out_page_id) {
    PageId pid = disk_manager_->AllocatePage();  // DiskManager has its own, separate thread-safety

    std::lock_guard<std::mutex> lock(mutex_);
    auto page = std::make_unique<Page>();  // std::array default-inits to all zero
    Page* raw = page.get();
    pages_[pid] = std::move(page);
    MarkDirtyLocked(pid);  // routes through MarkDirtyLocked (not a direct dirty_.insert) so the
                            // calling thread's transaction observer, if any, sees new pages too
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
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pages_.find(page_id);
    if (it != pages_.end()) return it->second.get();

    auto page = std::make_unique<Page>();
    disk_manager_->ReadPage(page_id, page->Data());
    Page* raw = page.get();
    pages_[page_id] = std::move(page);
    return raw;
}

void BufferPool::MarkDirtyLocked(PageId page_id) {
    dirty_.insert(page_id);
    auto it = observers_by_thread_.find(std::this_thread::get_id());
    if (it != observers_by_thread_.end() && it->second) it->second(page_id);
}

void BufferPool::MarkDirty(PageId page_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    MarkDirtyLocked(page_id);
}

void BufferPool::FlushAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (PageId pid : dirty_) {
        auto it = pages_.find(pid);
        if (it != pages_.end()) {
            disk_manager_->WritePage(pid, it->second->Data());
        }
    }
    dirty_.clear();
}

void BufferPool::FlushPages(const std::set<PageId>& page_ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (PageId pid : page_ids) {
        auto it = pages_.find(pid);
        if (it != pages_.end() && dirty_.count(pid) > 0) {
            disk_manager_->WritePage(pid, it->second->Data());
            dirty_.erase(pid);
        }
    }
}

size_t BufferPool::NumPagesInMemory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pages_.size();
}

size_t BufferPool::NumPagesOnDisk() const { return disk_manager_->NumPages(); }  // DiskManager locks its own state

void BufferPool::ResetAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    pages_.clear();
    dirty_.clear();
    disk_manager_->ResetFile();
}

void BufferPool::SetActiveTransactionObserver(std::function<void(PageId)> observer) {
    std::lock_guard<std::mutex> lock(mutex_);
    observers_by_thread_[std::this_thread::get_id()] = std::move(observer);
}

void BufferPool::ClearActiveTransactionObserver() {
    std::lock_guard<std::mutex> lock(mutex_);
    observers_by_thread_.erase(std::this_thread::get_id());
}

void BufferPool::DiscardPage(PageId page_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pages_.find(page_id);
    if (it == pages_.end()) return;  // never cached -- nothing to discard
    disk_manager_->ReadPage(page_id, it->second->Data());
    dirty_.erase(page_id);
}

}  // namespace flintdb
