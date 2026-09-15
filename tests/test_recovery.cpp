#include "../src/storage/disk_manager.h"
#include "../src/wal/recovery.h"
#include "test_framework.h"
#include "test_utils.h"

using namespace flintdb;
using flintdb::testing::TempFile;

namespace {

std::array<char, PAGE_SIZE> MakePageImage(char fill) {
    std::array<char, PAGE_SIZE> img;
    img.fill(fill);
    return img;
}

LogRecord MakeUpdate(Lsn lsn, TxnId txn_id, PageId page_id, char fill) {
    LogRecord r;
    r.lsn = lsn;
    r.type = LogRecordType::kUpdate;
    r.txn_id = txn_id;
    r.page_id = page_id;
    r.page_image = MakePageImage(fill);
    return r;
}

LogRecord MakeCommit(Lsn lsn, TxnId txn_id) {
    LogRecord r;
    r.lsn = lsn;
    r.type = LogRecordType::kCommit;
    r.txn_id = txn_id;
    return r;
}

LogRecord MakeBegin(Lsn lsn, TxnId txn_id) {
    LogRecord r;
    r.lsn = lsn;
    r.type = LogRecordType::kBegin;
    r.txn_id = txn_id;
    return r;
}

LogRecord MakeAbort(Lsn lsn, TxnId txn_id) {
    LogRecord r;
    r.lsn = lsn;
    r.type = LogRecordType::kAbort;
    r.txn_id = txn_id;
    return r;
}

}  // namespace

FLINTDB_TEST(recovery_of_empty_log_is_a_no_op) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    size_t replayed = RunRecovery({}, &dm);
    FLINTDB_CHECK_EQ(replayed, 0u);
}

FLINTDB_TEST(recovery_replays_a_committed_transactions_update) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    PageId pid = dm.AllocatePage();

    std::vector<LogRecord> records = {
        MakeBegin(1, 100),
        MakeUpdate(2, 100, pid, 'X'),
        MakeCommit(3, 100),
    };
    size_t replayed = RunRecovery(records, &dm);
    FLINTDB_CHECK_EQ(replayed, 1u);

    auto expected = MakePageImage('X');
    std::array<char, PAGE_SIZE> actual;
    dm.ReadPage(pid, actual.data());
    FLINTDB_CHECK(actual == expected);
}

FLINTDB_TEST(recovery_skips_updates_for_a_transaction_with_no_commit_record) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    PageId pid = dm.AllocatePage();
    // AllocatePage zero-fills via ftruncate; use that as the "untouched"
    // baseline to detect whether recovery incorrectly wrote to the page.
    std::array<char, PAGE_SIZE> zero{};

    std::vector<LogRecord> records = {
        MakeBegin(1, 200),
        MakeUpdate(2, 200, pid, 'Y'),
        // crash before Commit -- no commit record for txn 200
    };
    size_t replayed = RunRecovery(records, &dm);
    FLINTDB_CHECK_EQ(replayed, 0u);

    std::array<char, PAGE_SIZE> actual;
    dm.ReadPage(pid, actual.data());
    FLINTDB_CHECK(actual == zero);
}

FLINTDB_TEST(recovery_skips_an_aborted_transaction_even_if_it_somehow_had_updates_logged) {
    // TransactionManager::Abort never actually logs Update records in
    // practice (see recovery.h's doc comment: it discards the pages from
    // the buffer pool instead), but recovery's own correctness shouldn't
    // depend on that being true -- if an Update record exists for a txn
    // whose only terminal record is Abort (not Commit), it must still be
    // skipped, exactly like a txn with no terminal record at all.
    TempFile tmp;
    DiskManager dm(tmp.path());
    PageId pid = dm.AllocatePage();
    std::array<char, PAGE_SIZE> zero{};

    std::vector<LogRecord> records = {
        MakeBegin(1, 300),
        MakeUpdate(2, 300, pid, 'Z'),
        MakeAbort(3, 300),
    };
    size_t replayed = RunRecovery(records, &dm);
    FLINTDB_CHECK_EQ(replayed, 0u);

    std::array<char, PAGE_SIZE> actual;
    dm.ReadPage(pid, actual.data());
    FLINTDB_CHECK(actual == zero);
}

FLINTDB_TEST(recovery_applies_committed_updates_from_multiple_interleaved_transactions) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    PageId pid_a = dm.AllocatePage();
    PageId pid_b = dm.AllocatePage();

    std::vector<LogRecord> records = {
        MakeBegin(1, 1),
        MakeBegin(2, 2),
        MakeUpdate(3, 1, pid_a, 'A'),
        MakeUpdate(4, 2, pid_b, 'B'),
        MakeCommit(5, 1),
        // txn 2 never commits
    };
    size_t replayed = RunRecovery(records, &dm);
    FLINTDB_CHECK_EQ(replayed, 1u);

    auto expected_a = MakePageImage('A');
    std::array<char, PAGE_SIZE> actual_a;
    dm.ReadPage(pid_a, actual_a.data());
    FLINTDB_CHECK(actual_a == expected_a);

    std::array<char, PAGE_SIZE> zero{};
    std::array<char, PAGE_SIZE> actual_b;
    dm.ReadPage(pid_b, actual_b.data());
    FLINTDB_CHECK(actual_b == zero);
}

FLINTDB_TEST(recovery_of_multiple_updates_to_the_same_page_applies_them_in_lsn_order) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    PageId pid = dm.AllocatePage();

    // Same page updated by two different committed transactions -- the
    // later LSN must win, since that's genuinely what happened before
    // the crash.
    std::vector<LogRecord> records = {
        MakeUpdate(1, 1, pid, 'A'),
        MakeCommit(2, 1),
        MakeUpdate(3, 2, pid, 'B'),
        MakeCommit(4, 2),
    };
    size_t replayed = RunRecovery(records, &dm);
    FLINTDB_CHECK_EQ(replayed, 2u);

    auto expected = MakePageImage('B');
    std::array<char, PAGE_SIZE> actual;
    dm.ReadPage(pid, actual.data());
    FLINTDB_CHECK(actual == expected);
}

FLINTDB_TEST(recovery_is_idempotent_when_run_twice_over_the_same_records) {
    TempFile tmp;
    DiskManager dm(tmp.path());
    PageId pid = dm.AllocatePage();

    std::vector<LogRecord> records = {
        MakeUpdate(1, 1, pid, 'Q'),
        MakeCommit(2, 1),
    };
    RunRecovery(records, &dm);
    size_t replayed_again = RunRecovery(records, &dm);
    // Idempotent means "produces the same on-disk result", not "detects
    // and skips a repeat" -- recovery has no way to know it already ran,
    // and doesn't need to: writing the same bytes twice is harmless.
    FLINTDB_CHECK_EQ(replayed_again, 1u);

    auto expected = MakePageImage('Q');
    std::array<char, PAGE_SIZE> actual;
    dm.ReadPage(pid, actual.data());
    FLINTDB_CHECK(actual == expected);
}
