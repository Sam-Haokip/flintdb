# DECISIONS.md

A running log of every real design choice made on this project, the alternative(s) it beat, and why. Nothing here is rewritten silently — a decision later found wrong gets a new entry marked **RETRACTED** or **CORRECTED**, pointing back at the original, rather than being edited away.

Format per entry: date, decision, alternative(s) considered, why, status.

---

### D-001 — Engine implementation language: C++ (not Java)
- **Date:** 2026-09-15 (Phase 0)
- **Decision:** Build the engine itself in C++ (targeting C++17/20).
- **Alternative considered:** Java — the default suggested in the original project brief, with manual byte-layout management via `ByteBuffer`/`FileChannel` to keep it a real systems project rather than leaning on Java collections.
- **Why:** Explicit trade-off accepted going in: C++ has no garbage collector safety net, so a bug in page/buffer-pool management can corrupt memory or the on-disk file in ways Java's memory model would prevent by construction — that's a harder debugging path and more implementation time. In exchange, the raw-systems story is stronger for interviews (manual memory management, no GC pauses to explain away in benchmark numbers, closer to how SQLite and Postgres are actually built) and there's no ambiguity about whether performance numbers reflect the engine's design or the JVM's GC/JIT behavior.
- **Status:** active.

### D-002 — Build/dev environment: cloud workspace (not local machine, for now)
- **Date:** 2026-09-15 (Phase 0)
- **Decision:** Build and iterate in this session's cloud workspace rather than on the user's local machine.
- **Alternative considered:** Building directly on the user's MacBook via a connected folder, so the local toolchain/IDE/git config/GitHub auth is already in place.
- **Why:** No local folder was connected at the time this decision had to be made, and the cloud workspace has a full C++ toolchain, git, and network access available immediately with zero setup. This is not a permanent commitment — the repo is a normal git repo, so it can be cloned to the local machine (or a local folder can be connected and the work continued there) at any point without losing history. GitHub pushes only happen with explicit go-ahead each time, per the standing rules, regardless of which machine is doing the building.
- **Status:** active, reversible.

### D-003 — SQL statement surface includes explicit `CREATE INDEX` (not auto-indexing only)
- **Date:** 2026-09-15 (Phase 0)
- **Decision:** v1 grammar includes a `CREATE INDEX index_name ON table(column)` statement, producing a secondary B+-tree, in addition to the implicit primary-key clustering index.
- **Alternative considered:** Only ever index the primary key automatically, with no user-facing indexing statement.
- **Why:** Phase 5's planner has to make a real, defensible sequential-scan-vs-index-scan decision. That decision is more interesting and more clearly the user's own design choice — not just "the PK happened to be indexed" — if secondary indexes are something the schema author deliberately creates. It also gives Phase 2's index benchmark a second, realistic use case (looking up by a non-PK column) instead of only PK point-lookups.
- **Status:** active.

### D-004 — No-steal buffer pool + redo-only WAL as the default durability design
- **Date:** 2026-09-15 (Phase 0)
- **Decision:** Target a no-steal buffer-pool policy (dirty pages never flushed before their transaction commits) with redo-only WAL recovery, instead of full ARIES-style steal/force with undo+redo logging.
- **Alternative considered:** Steal/force (or steal/no-force) policies requiring undo logging and compensation log records — the standard production-database approach (what SQLite and Postgres actually do, in different forms).
- **Why:** No-steal removes the need for undo logging and crash-time compensation entirely — an aborted or uncommitted transaction's changes were never written to the data file, so there's nothing on disk to undo. That's a materially smaller, more provably-correct Phase 3, at the real cost of bounding a single transaction's dirty-page footprint to what fits in the buffer pool. Given the project's scope and timeline, that bound is an acceptable, explicitly-stated limitation rather than a hidden one.
- **Status:** active — flagged in SPEC.md §2 as revisable if Phase 3/7 benchmarking shows it doesn't hold up against a realistic workload; any change gets a new entry here, not an edit to this one.

### D-005 — v1 type system: `INTEGER` and `TEXT` only, no `NULL`
- **Date:** 2026-09-15 (Phase 0)
- **Decision:** Only two column types in v1 (`INTEGER` 64-bit signed, `TEXT` variable-length UTF-8), and no `NULL` value support — every column is implicitly `NOT NULL`.
- **Alternative considered:** Also supporting `REAL`/`FLOAT` and proper three-valued (`NULL`-aware) logic in predicates, joins, and comparisons, matching real SQL semantics more closely.
- **Why:** `NULL` handling is a disproportionate amount of correctness surface for its teaching value here — three-valued logic touches every comparator, every join, every aggregate-shaped feature (even though aggregates are already out of scope) — and it's not central to what this project is actually trying to demonstrate (storage engine, indexing, WAL, concurrency, query execution). Cutting it keeps Phases 1–7 focused on those. `REAL` is a smaller cut, deferred as stretch scope only after the v1 core (Phases 0–6) is fully working and tested — see SPEC.md §6.
- **Status:** active.
