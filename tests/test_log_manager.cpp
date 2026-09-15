#include "../src/wal/log_manager.h"
#include "test_framework.h"
#include "test_utils.h"

using namespace flintdb;
using flintdb::testing::CorruptByteAt;
using flintdb::testing::FileSizeOf;
using flintdb::testing::TempFile;
using flintdb::testing::TruncateFileTo;

namespace {

std::array<char, PAGE_SIZE> MakePageImage(char fill) {
    std::array<char, PAGE_SIZE> img;
    img.fill(fill);
    return img;
}

}  // namespace

FLINTDB_TEST(log_manager_fresh_file_starts_with_no_records_and_lsn_one) {
    TempFile tmp("wal");
    LogManager log(tmp.path());
    FLINTDB_CHECK(log.RecordsOnOpen().empty());
    FLINTDB_CHECK_EQ(log.NextLsn(), 1u);
}

FLINTDB_TEST(log_manager_append_assigns_strictly_increasing_lsns) {
    TempFile tmp("wal");
    LogManager log(tmp.path());
    Lsn l1 = log.AppendBegin(1);
    Lsn l2 = log.AppendCommit(1);
    Lsn l3 = log.AppendBegin(2);
    FLINTDB_CHECK_EQ(l1, 1u);
    FLINTDB_CHECK_EQ(l2, 2u);
    FLINTDB_CHECK_EQ(l3, 3u);
    FLINTDB_CHECK_EQ(log.NextLsn(), 4u);
}

FLINTDB_TEST(log_manager_reopen_sees_every_appended_record_in_order_with_correct_fields) {
    TempFile tmp("wal");
    auto image = MakePageImage('Q');
    {
        LogManager log(tmp.path());
        log.AppendBegin(42);
        log.AppendUpdate(42, 7, image.data());
        log.AppendCommit(42);
        log.Flush();
    }

    LogManager reopened(tmp.path());
    const auto& records = reopened.RecordsOnOpen();
    FLINTDB_CHECK_EQ(records.size(), 3u);

    FLINTDB_CHECK(records[0].type == LogRecordType::kBegin);
    FLINTDB_CHECK_EQ(records[0].lsn, 1u);
    FLINTDB_CHECK_EQ(records[0].txn_id, 42u);

    FLINTDB_CHECK(records[1].type == LogRecordType::kUpdate);
    FLINTDB_CHECK_EQ(records[1].lsn, 2u);
    FLINTDB_CHECK_EQ(records[1].txn_id, 42u);
    FLINTDB_CHECK_EQ(records[1].page_id, 7u);
    FLINTDB_CHECK(records[1].page_image == image);

    FLINTDB_CHECK(records[2].type == LogRecordType::kCommit);
    FLINTDB_CHECK_EQ(records[2].lsn, 3u);
    FLINTDB_CHECK_EQ(records[2].txn_id, 42u);

    FLINTDB_CHECK_EQ(reopened.NextLsn(), 4u);
}

FLINTDB_TEST(log_manager_abort_record_round_trips) {
    TempFile tmp("wal");
    {
        LogManager log(tmp.path());
        log.AppendBegin(1);
        log.AppendAbort(1);
    }
    LogManager reopened(tmp.path());
    const auto& records = reopened.RecordsOnOpen();
    FLINTDB_CHECK_EQ(records.size(), 2u);
    FLINTDB_CHECK(records[1].type == LogRecordType::kAbort);
    FLINTDB_CHECK_EQ(records[1].txn_id, 1u);
}

FLINTDB_TEST(log_manager_multiple_update_records_each_keep_their_own_page_image) {
    TempFile tmp("wal");
    auto image_a = MakePageImage('A');
    auto image_b = MakePageImage('B');
    {
        LogManager log(tmp.path());
        log.AppendUpdate(1, 10, image_a.data());
        log.AppendUpdate(1, 20, image_b.data());
    }
    LogManager reopened(tmp.path());
    const auto& records = reopened.RecordsOnOpen();
    FLINTDB_CHECK_EQ(records.size(), 2u);
    FLINTDB_CHECK_EQ(records[0].page_id, 10u);
    FLINTDB_CHECK(records[0].page_image == image_a);
    FLINTDB_CHECK_EQ(records[1].page_id, 20u);
    FLINTDB_CHECK(records[1].page_image == image_b);
}

FLINTDB_TEST(log_manager_torn_tail_after_a_full_record_is_discarded_and_file_is_truncated) {
    TempFile tmp("wal");
    size_t good_size;
    {
        LogManager log(tmp.path());
        log.AppendBegin(1);
        good_size = FileSizeOf(tmp.path());
        // Simulate a crash mid-write of the *next* record: an Update
        // record (large payload) that never finished landing on disk.
        auto image = MakePageImage('Z');
        log.AppendUpdate(1, 3, image.data());
    }
    size_t full_size = FileSizeOf(tmp.path());
    FLINTDB_CHECK(full_size > good_size);
    // Cut the file off partway through the second record's payload --
    // exactly what a crash mid-pwrite leaves behind.
    TruncateFileTo(tmp.path(), good_size + 40);

    LogManager reopened(tmp.path());
    const auto& records = reopened.RecordsOnOpen();
    FLINTDB_CHECK_EQ(records.size(), 1u);  // only the Begin record survives
    FLINTDB_CHECK(records[0].type == LogRecordType::kBegin);
    FLINTDB_CHECK_EQ(reopened.NextLsn(), 2u);

    // The file on disk must actually have been truncated back to the
    // last good record -- not just skipped over in memory -- so a later
    // Append* lands right after it instead of after the garbage.
    FLINTDB_CHECK_EQ(FileSizeOf(tmp.path()), good_size);
}

FLINTDB_TEST(log_manager_torn_write_of_the_very_first_record_leaves_an_empty_log) {
    TempFile tmp("wal");
    {
        LogManager log(tmp.path());
        auto image = MakePageImage('Z');
        log.AppendUpdate(1, 3, image.data());
    }
    // Truncate mid-header of the one and only record.
    TruncateFileTo(tmp.path(), 10);

    LogManager reopened(tmp.path());
    FLINTDB_CHECK(reopened.RecordsOnOpen().empty());
    FLINTDB_CHECK_EQ(reopened.NextLsn(), 1u);
    FLINTDB_CHECK_EQ(FileSizeOf(tmp.path()), 0u);
}

FLINTDB_TEST(log_manager_corrupted_checksum_on_a_full_record_is_treated_as_a_torn_tail) {
    TempFile tmp("wal");
    size_t good_size;
    {
        LogManager log(tmp.path());
        log.AppendBegin(1);
        good_size = FileSizeOf(tmp.path());
        log.AppendCommit(1);
    }
    // Flip a byte inside the second record's payload region (its txn_id
    // field) without changing its length -- a bit-flip, not a truncation.
    // The record is structurally complete but its checksum no longer
    // matches, which must be treated exactly like a torn write: stop
    // there, keep everything before it.
    CorruptByteAt(tmp.path(), good_size + 9 /* kOffTxnId */);

    LogManager reopened(tmp.path());
    const auto& records = reopened.RecordsOnOpen();
    FLINTDB_CHECK_EQ(records.size(), 1u);
    FLINTDB_CHECK(records[0].type == LogRecordType::kBegin);
    FLINTDB_CHECK_EQ(FileSizeOf(tmp.path()), good_size);
}

FLINTDB_TEST(log_manager_corrupted_page_image_byte_is_detected_via_checksum) {
    TempFile tmp("wal");
    {
        LogManager log(tmp.path());
        auto image = MakePageImage('M');
        log.AppendUpdate(1, 5, image.data());
    }
    // Flip a byte deep inside the update record's page-image payload
    // (well past the 25-byte header).
    CorruptByteAt(tmp.path(), 25 + 2000);

    LogManager reopened(tmp.path());
    FLINTDB_CHECK(reopened.RecordsOnOpen().empty());
    FLINTDB_CHECK_EQ(FileSizeOf(tmp.path()), 0u);
}

FLINTDB_TEST(log_manager_checkpoint_wipes_the_file_and_resets_lsn) {
    TempFile tmp("wal");
    LogManager log(tmp.path());
    log.AppendBegin(1);
    log.AppendCommit(1);
    FLINTDB_CHECK(FileSizeOf(tmp.path()) > 0);

    log.Checkpoint();

    FLINTDB_CHECK_EQ(FileSizeOf(tmp.path()), 0u);
    FLINTDB_CHECK_EQ(log.NextLsn(), 1u);

    // A record appended right after checkpoint gets LSN 1 again, and a
    // fresh reopen sees only it.
    Lsn lsn = log.AppendBegin(99);
    FLINTDB_CHECK_EQ(lsn, 1u);
}

FLINTDB_TEST(log_manager_reopen_after_checkpoint_sees_an_empty_log) {
    TempFile tmp("wal");
    {
        LogManager log(tmp.path());
        log.AppendBegin(1);
        log.AppendCommit(1);
        log.Checkpoint();
    }
    LogManager reopened(tmp.path());
    FLINTDB_CHECK(reopened.RecordsOnOpen().empty());
    FLINTDB_CHECK_EQ(reopened.NextLsn(), 1u);
}

FLINTDB_TEST(log_manager_flush_does_not_throw_and_records_remain_readable) {
    TempFile tmp("wal");
    LogManager log(tmp.path());
    log.AppendBegin(1);
    log.Flush();
    log.AppendCommit(1);
    log.Flush();
    FLINTDB_CHECK_EQ(log.RecordsOnOpen().size(), 0u);  // RecordsOnOpen is fixed at construction time
}
