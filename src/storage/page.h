#pragma once
#include "../common/config.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace flintdb {

// A single fixed-size page, laid out as a classic "slotted page":
//
//   [ 16-byte header | slot directory (grows right) | free space | record data (grows left) ]
//
// The header stores two boundaries: pd_lower (end of the slot directory)
// and pd_upper (start of the record-data area). Free space is whatever
// lies between them. Inserting a record writes its bytes just before
// pd_upper and appends a 4-byte (offset, length) slot entry just before
// pd_lower.
//
// Phase 1 has no per-page delete: slot ids are assigned once, in order,
// and never reused or shifted within a page's lifetime (HeapFile deletes
// by rewriting the whole file instead — see heap_file.h and
// docs/DECISIONS.md for why, and what changes in Phase 2).
class Page {
 public:
    static constexpr size_t kHeaderSize = 16;
    static constexpr size_t kSlotSize = 4;

    Page();

    // Resets this page to an empty heap page with the given id. Must be
    // called once before any Insert/Get calls on a freshly allocated page.
    void InitHeapPage(PageId page_id);

    PageId GetPageId() const;
    uint16_t GetSlotCount() const;

    // Inserts `data` as a new record. Returns the new slot id, or
    // std::nullopt if the page doesn't have enough contiguous free space
    // (the caller should try a different or a brand-new page).
    std::optional<SlotId> InsertRecord(const std::string& data);

    // Returns the bytes stored at `slot_id`. `slot_id` must be less than
    // GetSlotCount() (checked with an assertion in debug builds).
    std::string GetRecord(SlotId slot_id) const;

    // Raw access to the underlying buffer, for DiskManager/BufferPool to
    // move whole pages to and from disk without knowing about the slotted
    // layout.
    char* Data();
    const char* Data() const;

    // Bytes currently free between the slot directory and the record data.
    size_t FreeSpace() const;

 private:
    struct SlotEntry {
        uint16_t offset;
        uint16_t length;
    };

    void SetPageId(PageId v);
    void SetSlotCount(uint16_t v);
    uint32_t GetPdLower() const;
    void SetPdLower(uint16_t v);
    uint32_t GetPdUpper() const;
    void SetPdUpper(uint16_t v);

    SlotEntry GetSlotEntry(SlotId slot_id) const;
    void SetSlotEntry(SlotId slot_id, SlotEntry entry);

    std::array<char, PAGE_SIZE> buf_{};
};

}  // namespace flintdb
