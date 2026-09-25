// Phase 7 benchmark (task #8, docs/DECISIONS.md D-055+): a defined
// insert/read workload, run against the real engine through the real SQL
// layer (Parse()+Execute()/ExecuteInCurrentTransaction(), the same path
// tools/flintdb_cli.cpp and every SQL-level test use), measured three
// different ways to find where a real bottleneck actually is rather than
// assuming one:
//
//   commit_granularity -- same N inserted rows, same table, same
//   process, the only difference is transaction boundaries: N separate
//   autocommit statements (N fsyncs, docs/SPEC.md section 2 -- every
//   commit calls LogManager::Flush()) vs. one explicit transaction
//   wrapping all N (one fsync). Isolates per-commit fsync overhead.
//
//   size_scaling -- single-threaded insert and point-SELECT latency,
//   timed individually, against a table already pre-loaded to N rows.
//   Run at several N by the Python driver to see whether per-op cost
//   stays flat as N grows (expected for a working B+-tree index) or
//   creeps up (which would point at BufferPool's no-eviction growth --
//   src/storage/buffer_pool.h's own comment already flags this as an
//   unmeasured Phase-1 simplification).
//
//   concurrency -- N worker threads, each running its own autocommit
//   inserts against a shared, pre-loaded table, started together and
//   timed as one batch. Run at several thread counts by the Python
//   driver to see whether throughput scales with threads or flattens --
//   BufferPool's single pool-wide mutex is held across a cold FetchPage/
//   NewPage's disk I/O (buffer_pool.h's own comment already calls this
//   out as "a deliberate scope simplification"), and BPlusTree's
//   synthetic root-lock sentinel (D-028) serializes every structural
//   operation at the root -- either is a plausible, already-self-flagged
//   candidate for why throughput might not scale.
//
// Usage:
//   bench_workload commit_granularity <batch_size> <num_trials> [seed]
//   bench_workload size_scaling <preload_n> <num_ops> [seed]
//   bench_workload concurrency <preload_n> <num_threads> <ops_per_thread> [seed]
//
// Output: CSV to stdout, one row per timed sample (or per trial, for
// concurrency, since that mode measures one aggregate wall-clock time
// per batch of concurrent work rather than individual op latencies).
#include "../src/db/database.h"
#include "../src/sql/parser.h"
#include "../src/sql/session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace flintdb;

namespace {

// A unique, self-deleting temp directory -- the bench binary's own small
// copy of tests/test_utils.h's TempDir, kept local rather than shared so
// bench/ doesn't pick up a dependency on tests/ (mirroring
// bench_index_vs_scan.cpp's existing choice to build its own temp paths
// with getpid() rather than reach into tests/ for one).
class TempDir {
 public:
    explicit TempDir(const std::string& tag) {
        auto dir = std::filesystem::temp_directory_path();
        path_ = (dir / ("flintdb_bench_" + tag + "_" + std::to_string(getpid()) + "_" +
                         std::to_string(counter_++)))
                    .string();
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::string& path() const { return path_; }

 private:
    std::string path_;
    static inline int counter_ = 0;
};

std::string MakeText(std::mt19937& gen, size_t len) {
    // A fixed-length payload column alongside the indexed integer key --
    // realistic enough (a real row has more than just its key) without
    // needing a whole schema of column kinds. Random per row (not a
    // constant) so nothing about it could plausibly get constant-folded
    // or specially cached.
    static const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::uniform_int_distribution<size_t> pick(0, sizeof(kAlphabet) - 2);
    std::string s(len, ' ');
    for (size_t i = 0; i < len; i++) s[i] = kAlphabet[pick(gen)];
    return s;
}

ExecuteResult Run(Database& db, const std::string& sql) { return Execute(db, Parse(sql)); }

// Bulk-preloads `n` rows (keys 0..n-1, in random insertion order -- not
// sorted, matching bench_index_vs_scan.cpp's own reasoning for why that
// matters) into a fresh table `t` in `db`, as one explicit transaction so
// this setup cost is cheap and never itself part of a timed region.
void Preload(Database& db, const std::string& table, size_t n, std::mt19937& gen) {
    Run(db, "CREATE TABLE " + table + " (id INTEGER PRIMARY KEY, payload TEXT)");
    std::vector<int64_t> keys(n);
    for (size_t i = 0; i < n; i++) keys[i] = static_cast<int64_t>(i);
    std::shuffle(keys.begin(), keys.end(), gen);

    Transaction* txn = db.GetTransactionManager().Begin();
    for (int64_t k : keys) {
        std::string sql =
            "INSERT INTO " + table + " VALUES (" + std::to_string(k) + ", '" + MakeText(gen, 20) + "')";
        ExecuteInCurrentTransaction(db, Parse(sql));
    }
    db.GetTransactionManager().Commit(txn);
}

int64_t NowMicros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::high_resolution_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------------

int RunCommitGranularity(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: bench_workload commit_granularity <batch_size> <num_trials> [seed]\n";
        return 2;
    }
    size_t batch_size = static_cast<size_t>(std::stoul(argv[2]));
    int num_trials = std::stoi(argv[3]);
    unsigned seed = argc > 4 ? static_cast<unsigned>(std::stoul(argv[4])) : 42u;
    std::mt19937 gen(seed);

    std::cout << "mode,batch_size,trial,microseconds\n";

    for (int t = 0; t < num_trials; t++) {
        TempDir dir("commit_gran_" + std::to_string(t));
        std::filesystem::create_directory(dir.path());
        Database db(dir.path());
        Run(db, "CREATE TABLE t_auto (id INTEGER PRIMARY KEY, payload TEXT)");
        Run(db, "CREATE TABLE t_explicit (id INTEGER PRIMARY KEY, payload TEXT)");

        // (a) autocommit: batch_size separate implicit transactions, each
        // with its own fsync (session.h's Execute -> D-049's autocommit).
        {
            auto start = NowMicros();
            for (size_t i = 0; i < batch_size; i++) {
                std::string sql =
                    "INSERT INTO t_auto VALUES (" + std::to_string(i) + ", '" + MakeText(gen, 20) + "')";
                Run(db, sql);
            }
            auto end = NowMicros();
            std::cout << "autocommit," << batch_size << "," << t << "," << (end - start) << "\n";
        }

        // (b) explicit: one transaction wrapping the whole batch, one
        // fsync total, same number of rows inserted.
        {
            auto start = NowMicros();
            Transaction* txn = db.GetTransactionManager().Begin();
            for (size_t i = 0; i < batch_size; i++) {
                std::string sql =
                    "INSERT INTO t_explicit VALUES (" + std::to_string(i) + ", '" + MakeText(gen, 20) + "')";
                ExecuteInCurrentTransaction(db, Parse(sql));
            }
            db.GetTransactionManager().Commit(txn);
            auto end = NowMicros();
            std::cout << "explicit," << batch_size << "," << t << "," << (end - start) << "\n";
        }
    }
    return 0;
}

int RunSizeScaling(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: bench_workload size_scaling <preload_n> <num_ops> [seed]\n";
        return 2;
    }
    size_t preload_n = static_cast<size_t>(std::stoul(argv[2]));
    size_t num_ops = static_cast<size_t>(std::stoul(argv[3]));
    unsigned seed = argc > 4 ? static_cast<unsigned>(std::stoul(argv[4])) : 42u;
    std::mt19937 gen(seed);

    TempDir dir("size_scaling");
    std::filesystem::create_directory(dir.path());
    Database db(dir.path());
    Preload(db, "t", preload_n, gen);

    std::cout << "mode,preload_n,trial,microseconds\n";

    // Point-SELECTs first, on keys guaranteed to already exist (0..preload_n-1)
    // and picked before any timed insert below changes what "existing" means.
    std::uniform_int_distribution<int64_t> pick_existing(0, preload_n == 0 ? 0 : static_cast<int64_t>(preload_n) - 1);
    for (size_t i = 0; i < num_ops; i++) {
        int64_t key = preload_n == 0 ? 0 : pick_existing(gen);
        std::string sql = "SELECT * FROM t WHERE id = " + std::to_string(key);
        auto start = NowMicros();
        ExecuteResult r = Run(db, sql);
        auto end = NowMicros();
        if (preload_n > 0 && r.rows.empty()) {
            std::cerr << "bench error: select did not find preloaded key " << key << "\n";
            return 1;
        }
        std::cout << "select," << preload_n << "," << i << "," << (end - start) << "\n";
    }

    // Then inserts of brand-new keys (preload_n, preload_n+1, ...), each
    // its own autocommit statement -- the realistic per-statement cost a
    // client actually pays, not an artificially cheap batched insert.
    for (size_t i = 0; i < num_ops; i++) {
        int64_t key = static_cast<int64_t>(preload_n + i);
        std::string sql = "INSERT INTO t VALUES (" + std::to_string(key) + ", '" + MakeText(gen, 20) + "')";
        auto start = NowMicros();
        Run(db, sql);
        auto end = NowMicros();
        std::cout << "insert," << preload_n << "," << i << "," << (end - start) << "\n";
    }
    return 0;
}

int RunConcurrency(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: bench_workload concurrency <preload_n> <num_threads> <ops_per_thread> [seed]\n";
        return 2;
    }
    size_t preload_n = static_cast<size_t>(std::stoul(argv[2]));
    int num_threads = std::stoi(argv[3]);
    size_t ops_per_thread = static_cast<size_t>(std::stoul(argv[4]));
    unsigned seed = argc > 5 ? static_cast<unsigned>(std::stoul(argv[5])) : 42u;
    std::mt19937 gen(seed);

    TempDir dir("concurrency");
    std::filesystem::create_directory(dir.path());
    Database db(dir.path());
    Preload(db, "t", preload_n, gen);

    std::cout << "mode,num_threads,trial,total_microseconds,total_ops,total_retries\n";

    // Each thread inserts into its own disjoint key range (partitioned by
    // thread index), so no thread can ever collide with another's
    // PRIMARY KEY -- what's being measured is contention on shared engine
    // state (BufferPool's mutex, BPlusTree's root sentinel, the WAL file),
    // not artificial application-level lock contention from sharing keys.
    //
    // First real finding, before any timing even matters: at more than
    // one thread, an autocommit INSERT can throw TransactionAbortedException
    // -- BPlusTree's root-lock sentinel (docs/DECISIONS.md D-028) makes
    // every structural index operation acquire the *same* synthetic lock,
    // so two threads inserting at the same time, even into completely
    // disjoint key ranges touching disjoint leaves, still contend on that
    // one sentinel, and wait-die (D-022) kills the younger of the two
    // rather than let it wait. session.h's own Execute() already cleans
    // up (catches, Aborts, rethrows -- session.cpp) so this is safe to
    // retry, exactly the way transaction.h's own AcquireLock comment says
    // a caller must: catch it and retry the whole statement in a fresh
    // transaction, not continue using the one that just lost its lock
    // request. std::atomic<size_t> total_retries counts how often this
    // actually happens, since that count is itself part of what this
    // benchmark is measuring.
    std::atomic<size_t> total_retries{0};
    std::vector<std::thread> workers;
    auto start = NowMicros();
    for (int w = 0; w < num_threads; w++) {
        workers.emplace_back([&db, &total_retries, w, preload_n, ops_per_thread]() {
            int64_t base = static_cast<int64_t>(preload_n) + static_cast<int64_t>(w) * 10'000'000LL;
            std::mt19937 local_gen(1000u + static_cast<unsigned>(w));
            for (size_t i = 0; i < ops_per_thread; i++) {
                int64_t key = base + static_cast<int64_t>(i);
                std::string sql = "INSERT INTO t VALUES (" + std::to_string(key) + ", '" + MakeText(local_gen, 20) +
                                   "')";
                while (true) {
                    try {
                        Run(db, sql);
                        break;
                    } catch (const TransactionAbortedException&) {
                        total_retries.fetch_add(1, std::memory_order_relaxed);
                        // no backoff: wait-die's own correctness argument
                        // (D-022) doesn't depend on one, and this
                        // benchmark wants to measure raw contention cost,
                        // not a backoff policy's effect on it.
                    }
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    auto end = NowMicros();

    size_t total_ops = static_cast<size_t>(num_threads) * ops_per_thread;
    std::cout << "concurrency," << num_threads << ",0," << (end - start) << "," << total_ops << ","
              << total_retries.load() << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bench_workload <commit_granularity|size_scaling|concurrency> ...\n";
        return 2;
    }
    std::string mode = argv[1];
    if (mode == "commit_granularity") return RunCommitGranularity(argc, argv);
    if (mode == "size_scaling") return RunSizeScaling(argc, argv);
    if (mode == "concurrency") return RunConcurrency(argc, argv);
    std::cerr << "unknown mode: " << mode << "\n";
    return 2;
}
