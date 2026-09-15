#include "buffer_pool.h"

namespace flintdb {

BufferPool::BufferPool(DiskManager* disk_manager) : disk_manager_(disk_manager) {}

Page* BufferPool::NewHeapPage(PageId* out_page_id) {
    PageId pid = disk_manager_->AllocatePage();
    auto page = std::make_unique<Page>();
    page->InitHeapPage(pid);
    Page* raw = page.get();
    pages_[pid] = std::move(page);
    dirty_.insert(pid);
    if (out_page_id) *out_page_id = pid;
    return raw;
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

void BufferPool::MarkDirty(PageId page_id) { dirty_.insert(page_id); }

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

}  // namespace flintdb
