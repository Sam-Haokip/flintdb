#include "page.h"

#include <cassert>
#include <cstring>

namespace flintdb {

namespace {

constexpr size_t kOffPageId = 0;      // uint32_t
constexpr size_t kOffPageType = 4;    // uint16_t
constexpr size_t kOffSlotCount = 6;   // uint16_t
constexpr size_t kOffPdLower = 8;     // uint16_t
constexpr size_t kOffPdUpper = 10;    // uint16_t
// bytes 12..15 reserved (future use: e.g. a page checksum for Phase 6's
// corruption-fuzzing work — deliberately zeroed and unused for now rather
// than designed blind).

// memcpy-based accessors rather than reinterpret_cast'ing buf_.data() to a
// header struct pointer: the latter relies on the buffer having the right
// alignment and on strict-aliasing not biting us, which is exactly the
// kind of "technically UB, works on my machine" shortcut a from-scratch
// storage engine shouldn't take. memcpy is always well-defined and, for
// these tiny fixed sizes, compiles down to the same load/store anyway.
template <typename T>
T ReadAt(const char* buf, size_t offset) {
    T v;
    std::memcpy(&v, buf + offset, sizeof(T));
    return v;
}

template <typename T>
void WriteAt(char* buf, size_t offset, T v) {
    std::memcpy(buf + offset, &v, sizeof(T));
}

}  // namespace

Page::Page() = default;

void Page::InitHeapPage(PageId page_id) {
    buf_.fill(0);
    SetPageId(page_id);
    WriteAt<uint16_t>(buf_.data(), kOffPageType, static_cast<uint16_t>(PageType::kHeap));
    SetSlotCount(0);
    SetPdLower(static_cast<uint16_t>(kHeaderSize));
    SetPdUpper(static_cast<uint16_t>(PAGE_SIZE));
}

PageId Page::GetPageId() const { return ReadAt<PageId>(buf_.data(), kOffPageId); }
void Page::SetPageId(PageId v) { WriteAt<PageId>(buf_.data(), kOffPageId, v); }

uint16_t Page::GetSlotCount() const { return ReadAt<uint16_t>(buf_.data(), kOffSlotCount); }
void Page::SetSlotCount(uint16_t v) { WriteAt<uint16_t>(buf_.data(), kOffSlotCount, v); }

uint32_t Page::GetPdLower() const { return ReadAt<uint16_t>(buf_.data(), kOffPdLower); }
void Page::SetPdLower(uint16_t v) { WriteAt<uint16_t>(buf_.data(), kOffPdLower, v); }

uint32_t Page::GetPdUpper() const { return ReadAt<uint16_t>(buf_.data(), kOffPdUpper); }
void Page::SetPdUpper(uint16_t v) { WriteAt<uint16_t>(buf_.data(), kOffPdUpper, v); }

Page::SlotEntry Page::GetSlotEntry(SlotId slot_id) const {
    size_t off = kHeaderSize + static_cast<size_t>(slot_id) * kSlotSize;
    SlotEntry e;
    e.offset = ReadAt<uint16_t>(buf_.data(), off);
    e.length = ReadAt<uint16_t>(buf_.data(), off + sizeof(uint16_t));
    return e;
}

void Page::SetSlotEntry(SlotId slot_id, SlotEntry entry) {
    size_t off = kHeaderSize + static_cast<size_t>(slot_id) * kSlotSize;
    WriteAt<uint16_t>(buf_.data(), off, entry.offset);
    WriteAt<uint16_t>(buf_.data(), off + sizeof(uint16_t), entry.length);
}

std::optional<SlotId> Page::InsertRecord(const std::string& data) {
    if (data.size() > UINT16_MAX) return std::nullopt;
    uint16_t len = static_cast<uint16_t>(data.size());

    uint32_t pd_lower = GetPdLower();
    uint32_t pd_upper = GetPdUpper();
    if (pd_upper < pd_lower) return std::nullopt;  // corrupt-page guard

    size_t free_space = pd_upper - pd_lower;
    size_t needed = static_cast<size_t>(len) + kSlotSize;
    if (free_space < needed) return std::nullopt;

    uint16_t new_pd_upper = static_cast<uint16_t>(pd_upper - len);
    if (len > 0) {
        std::memcpy(buf_.data() + new_pd_upper, data.data(), len);
    }

    uint16_t slot_count = GetSlotCount();
    SlotId slot_id = slot_count;
    SetSlotEntry(slot_id, SlotEntry{new_pd_upper, len});
    SetSlotCount(static_cast<uint16_t>(slot_count + 1));
    SetPdLower(static_cast<uint16_t>(pd_lower + kSlotSize));
    SetPdUpper(new_pd_upper);
    return slot_id;
}

std::string Page::GetRecord(SlotId slot_id) const {
    assert(slot_id < GetSlotCount());
    SlotEntry e = GetSlotEntry(slot_id);
    return std::string(buf_.data() + e.offset, e.length);
}

char* Page::Data() { return buf_.data(); }
const char* Page::Data() const { return buf_.data(); }

size_t Page::FreeSpace() const {
    uint32_t pd_lower = GetPdLower();
    uint32_t pd_upper = GetPdUpper();
    if (pd_upper < pd_lower) return 0;
    return pd_upper - pd_lower;
}

}  // namespace flintdb
