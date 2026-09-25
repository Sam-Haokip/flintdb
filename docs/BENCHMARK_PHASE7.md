# Phase 7 benchmark: commit granularity and concurrent-insert contention

Methodology: `bench/bench_workload.cpp` drives a real `Database` through the real SQL layer (`Parse()`+`Execute()`/`ExecuteInCurrentTransaction()`, the same path every SQL-level test and `tools/flintdb_cli.cpp` use), not a synthetic microbenchmark against raw storage internals. Built with the project's default `build/` (CMake `Debug`, `-g`, no optimization) -- the same build used for `docs/BENCHMARK_PHASE2.md`, kept consistent rather than switched to `Release` so the two reports are comparable. Every number below is a mean +/- one standard deviation across repeated trials, per this project's standing rule (`docs/SPEC.md` section 5) -- except the final "pushed harder" section, which is explicitly a single-trial illustrative measurement instead, because that regime takes roughly a minute per trial by design (see that section's own note).

The concurrency and commit-granularity sections were each measured twice: once against this code's actual current state (`AFTER`, D-055's `LogManager::FlushThrough` group commit), and once against `src/txn/transaction_manager.cpp` temporarily reverted to a plain, unconditional `Flush()` per commit (`BEFORE`, i.e. what every commit did prior to D-055) -- produced by `git stash push -- src/txn/transaction_manager.cpp`, a rebuild, the BEFORE measurement, then `git stash pop` to restore. `log_manager.h`/`.cpp` (where `FlushThrough` itself lives) are untouched either way; only the caller changes.

Run: `python3 scripts/bench_workload.py` (builds `bench_workload` and the full test suite itself as needed).

## 1. Single-threaded latency across table size

The workload's baseline shape, at a few data sizes, before looking at concurrency or commit granularity at all: a table preloaded to N rows (random insertion order), then 100 point-SELECTs on existing keys and 100 autocommit INSERTs of new keys, each timed individually. Measured once against the current (AFTER) build only -- both operations are single-threaded and, like section 2's `explicit` column, pay exactly one fsync each regardless of group commit, so this dimension is not expected to (and, per section 2, does not) move with the D-055 fix.

| preload_n | select mean (us) | select stdev | insert mean (us) | insert stdev |
|---:|---:|---:|---:|---:|
| 100 | 291.0 | 70.7 | 401.8 | 51.6 |
| 1000 | 337.8 | 90.6 | 465.9 | 82.1 |
| 10000 | 323.3 | 58.4 | 525.8 | 60.7 |

Finding: per-op latency is flat as the table grows across this range -- no evidence here of `BufferPool`'s no-eviction growth (`src/storage/buffer_pool.h`) or B+-tree depth showing up as a single-threaded cost yet. This is also the evidence for section 4's claim that the B+-tree root-lock contention finding is not a small-tree artifact: if larger trees were the problem, it would show here first, as rising single-threaded latency, and it does not.

## 2. Commit granularity: per-transaction fsync cost, before vs. after group commit

Same rows inserted into the same fresh table in the same process, two ways: `autocommit` issues each row as its own separate SQL statement (D-049's autocommit policy wraps each in its own implicit transaction -- one fsync per row); `explicit` wraps the whole batch in one `BEGIN`...`COMMIT` (one fsync total). This isolates per-commit fsync overhead as the only difference between the two columns, and is run against both the BEFORE and AFTER builds to check whether group commit changes it.

| batch | BEFORE autocommit (us) | BEFORE explicit (us) | BEFORE speedup | AFTER autocommit (us) | AFTER explicit (us) | AFTER speedup |
|---:|---:|---:|---:|---:|---:|---:|
| 10 | 5158.4 +/- 602.6 | 876.9 +/- 152.2 | 5.9x | 4855.2 +/- 769.6 | 781.3 +/- 120.9 | 6.2x |
| 50 | 21146.4 +/- 3864.4 | 1860.9 +/- 93.8 | 11.4x | 20691.2 +/- 2626.6 | 1863.0 +/- 232.0 | 11.1x |

Finding (negative result, kept in per this project's standing rule): per-commit fsync cost dominates small, sequential-transaction latency by roughly an order of magnitude, both BEFORE and AFTER -- group commit does not measurably change this number, and by design should not: `FlushThrough` only coalesces fsyncs across *concurrently arriving* commits, and every commit here runs sequentially, alone, on one thread, so each one still pays its own fsync regardless of the coalescing mechanism sitting underneath it. This motivated looking at concurrent commits instead (section 3), which is where group commit actually has something to coalesce.

## 3. Concurrent insert throughput, before vs. after group commit (D-055)

Three thread counts, each against a table preloaded to 1000 rows, 20 inserts per thread into a disjoint key range per thread (so no two threads can ever collide on a PRIMARY KEY -- what's measured is contention on shared engine state, not application-level key contention), 10 repeated trials per data point. `retries` counts `TransactionAbortedException` retries (wait-die, D-022, killing the younger of two transactions contending for the same lock) -- see section 4 for why this is nonzero at all even though every thread inserts into its own disjoint key range.

| threads | BEFORE throughput (ops/s) | BEFORE retries | AFTER throughput (ops/s) | AFTER retries | throughput change |
|---:|---:|---:|---:|---:|---:|
| 1 | 2217.0 +/- 187.7 | 0.0 +/- 0.0 | 1975.0 +/- 220.1 | 0.0 +/- 0.0 | -10.9% |
| 2 | 1206.1 +/- 207.3 | 470.9 +/- 94.7 | 1413.2 +/- 139.5 | 397.7 +/- 83.0 | +17.2% |
| 4 | 508.3 +/- 67.9 | 2493.8 +/- 310.6 | 643.0 +/- 60.1 | 2135.0 +/- 229.3 | +26.5% |

Finding: at 2 and 4 threads, group commit measurably helps, and the improvement *grows* with thread count (+17.2% throughput at 2 threads, +26.5% at 4) -- fewer, shorter fsync waits mean shorter critical sections, so fewer wait-die collisions on the shared state every insert contends on (see section 4), and that effect compounds as more threads are actually contending. At 1 thread there is nothing to coalesce -- no concurrent commit ever arrives while a flush is in flight -- and the mean throughput is actually slightly *lower* (-10.9%; BEFORE and AFTER's one-stdev ranges overlap, so this is not a strong claim, just an honest one): `FlushThrough` takes its own `flush_mutex_`, checks `flush_in_progress_`, and updates `durable_lsn_` around the same fsync call that `Flush()` used to make directly, and that bookkeeping is pure overhead when there's no concurrent commit to fold in. Section 4 pushes further and shows what dominates once contention gets severe.

## 4. Pushing the workload harder: a deeper, separate bottleneck found and *not* fixed here

The brief's own instruction ("push the workload harder if the first attempt doesn't surface one") surfaces something far more severe than fsync coalescing once thread count climbs to 8. This is a single-trial illustrative measurement, not repeated-and-averaged like every other number in this report -- each trial at 8 threads takes upward of a minute *because of* the collapse being measured, which makes a proper many-trial variance run impractically slow for routine reruns of this report. The qualitative finding (throughput collapses by two to three orders of magnitude, not merely degrades) is unambiguous regardless of trial count.

| build | threads | total wall time | total ops | total retries | retries per successful op |
|---|---:|---:|---:|---:|---:|
| BEFORE | 8 | 36.55s | 160 | 638022 | 3988 |
| AFTER | 8 | 32.56s | 160 | 579684 | 3623 |

Group commit's benefit is still measurable here (+12.2% throughput, -9.1% retries -- consistent with, if smaller than, section 3's 4-thread improvement) but it is dwarfed by the scale of the collapse itself: both BEFORE and AFTER take 30+ seconds and roughly half a million retries to insert 160 rows, which should take a few dozen milliseconds. A double-digit-percent improvement on top of a two-to-three-order-of-magnitude collapse is not a fix for the collapse -- it isn't the bottleneck at this concurrency level.

Root cause (see `docs/DECISIONS.md` D-055 for the full investigation): `BPlusTree::Insert`/`InsertRecursive` acquire an *exclusive* lock on every node from the root down to the target leaf, through the normal transactional `LockManager` (Strict 2PL -- every lock held until commit, `docs/SPEC.md` section 3), with no early release once a node is proven safe from needing to split. Every single insert into a given index therefore exclusively locks whatever page currently serves as that index's actual root node, for its *entire* transaction duration -- regardless of tree depth or how far apart two inserts' actual target keys are. Section 1's flat single-threaded latency across table size is the evidence this is not a small-tree artifact (few leaves to spread load across): a tree large enough to have grown past its first couple of levels is still shallow enough that every insert touches the same one or two internal nodes anyway, so the collapse doesn't meaningfully improve as the table grows.

This is not what D-055 fixes, and fixing it properly is out of scope for this phase: the correct fix is lock coupling / latch crabbing -- separating the B+-tree's *physical structural* protection (preventing two concurrent splits from corrupting the tree's pointers) from its *logical transactional* protection (the actual isolation guarantee `docs/SPEC.md` section 3 makes over row data), so a structural latch on an internal node can be released the moment a traversal proves that node won't need to split, instead of being held for the rest of the transaction the way every lock currently is. That is a substantially larger, correctness-sensitive redesign of `src/index/btree.cpp`'s locking discipline, not a bounded bug fix, and rushing it under this phase's time budget risks introducing exactly the class of subtle concurrency bug this project's own history (D-036/D-041/D-042/D-043, D-054) has repeatedly found expensive to track down after the fact. Documented here, precisely characterized and measured, as deliberate future work -- not a gap this report is unaware of.
