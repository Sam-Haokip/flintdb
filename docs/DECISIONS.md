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

### D-006 — GitHub push cadence and mechanism: push after every phase, from the local machine, via a scoped PAT
- **Date:** 2026-09-15 (Phase 0)
- **Decision:** Supersedes the "push only with explicit go-ahead each call" default: standing go-ahead to push after every phase completes, not just at the end. Development still happens in the cloud workspace (D-002 unchanged — a multi-week, multi-phase systems build is exactly the case the "long multi-step build" exception calls out for staying in the persistent cloud environment). At each phase boundary, the whole repo is synced to `~/projects/flintdb` on the user's Mac and pushed from there, using a GitHub Personal Access Token, rather than pushed directly from the cloud workspace.
- **Alternatives considered:** (a) push directly from the cloud workspace using a token stored there; (b) have the user run `git push` manually after each phase, with no token shared at all.
- **Why not (a):** the user explicitly chose "push from your local machine" when asked how to authenticate.
- **Important correction discovered while setting this up:** the "local machine" reachable via `device_bash` is an isolated sandbox VM the desktop app mounts the connected folder into — not the user's actual Terminal/shell. It has no pre-existing git credential helper, no SSH key, no `gh` CLI, and DNS/SSH egress is blocked (only HTTPS is allowed by the egress policy, confirmed via `curl` to github.com/api.github.com succeeding while `ssh git@github.com` failed to resolve). So "push from local machine" does not avoid needing a token — it still needs a PAT, just used from `~/projects/flintdb` on the Mac instead of the cloud workspace. Local git identity in that repo is set (not globally) to `Samuel Haokip <samuelhaok@gmail.com>`, matching the standing authorship rule.
- **Why not (b):** the user asked for pushes after every phase, which implies I do the pushing, not a manual step on their end every time.
- **Status:** active. Still needs the user to create the empty `flintdb` GitHub repo and provide a scoped PAT before the first push can happen (tracked as an open item, not yet done as of this entry).

### D-007 — Standing rule: run the full test suite at the end of every phase, and fix anything it finds before moving on
- **Date:** 2026-09-15 (start of Phase 1)
- **Decision:** After every phase's code is written, run the complete test suite (not just the new phase's tests) and, if anything fails, diagnose and fix it — either the code or the test, whichever is actually wrong — before considering the phase done or moving to the next one.
- **Alternative considered:** Run tests as a matter of course but only report results, leaving fixes for the user to request explicitly.
- **Why:** Explicit user instruction. It also matches the project's own stated verification standard (SPEC.md §5: "no claim in this project counts until it's measured or tested") and the "no unexplained code" / "treat a suspiciously easy green run as a reason to look harder" standing rules from the original brief.
- **Status:** active, applies to every subsequent phase.

### D-008 — On-disk page layout: 4096-byte slotted pages, memcpy-based header access
- **Date:** 2026-09-15 (Phase 1)
- **Decision:** Fixed 4096-byte pages, laid out as a slotted page: a 16-byte header, a slot directory growing forward from the header (4 bytes per slot: 2-byte offset + 2-byte length), and record data growing backward from the end of the page. Header/slot fields are read and written with `memcpy` into local variables rather than `reinterpret_cast`ing the raw buffer to a header struct pointer.
- **Alternative considered:** A fixed-size-row page format (no slot indirection, records addressed by a computed offset from a row number) — simpler, but only works if every row in a table is the same byte size, which `TEXT` columns rule out immediately. Also considered `reinterpret_cast<Header*>(buf)` for header access — faster to write, but relies on buffer alignment and strict-aliasing guarantees that aren't actually guaranteed for a `char` buffer read from disk, which is exactly the kind of "probably fine in practice" shortcut a from-scratch storage engine shouldn't take when the honest, portable alternative (`memcpy`) costs nothing measurable at this page size.
- **Why:** Slotted pages are the standard answer to variable-length records (this is what SQLite and Postgres both do, in different flavors) and directly support the RID model (`page_id` + `slot_id`) that Phase 2's B-tree will need to point at. 4096 bytes matches the common OS page size — a conventional, defensible default; revisit with a real measurement if Phase 2's fanout benchmark gives a reason to.
- **Status:** active. Verified with unit tests (`tests/test_page.cpp`) and a full test run under AddressSanitizer + UndefinedBehaviorSanitizer + leak detection (clean, no findings) — worth calling out specifically here since this is exactly the class of bug (buffer overrun, misaligned access, UB) that C++ was chosen over Java to accept the risk of (see D-001).

### D-009 — In-house minimal test framework instead of GoogleTest/Catch2
- **Date:** 2026-09-15 (Phase 1)
- **Decision:** Wrote a ~60-line self-registering test harness (`tests/test_framework.h`: a `FLINTDB_TEST` macro, two assertion macros, one runner) rather than depending on an external C++ test framework.
- **Alternative considered:** GoogleTest or Catch2 — both mature, both would work fine, both are completely standard choices for a project like this.
- **Why:** With the C++20 build already dependency-free (no package manager, no vendored libraries), pulling in a real framework would be the single external dependency in the whole build — for a project whose entire point is owning the hard parts from scratch, a test harness this small is easy to fully own, compiles instantly, and isn't worth trading that dependency-free build for. This is a close call, not an obviously-correct choice — a reasonable engineer could pick GoogleTest here and be right to. If test needs grow past what these ~60 lines comfortably support (parameterized tests, fixtures, mocking), that's a real reason to revisit and switch, logged as a new entry rather than silently swapped in.
- **Status:** active.

### D-010 — HeapFile::Delete implemented as a full-file rewrite; RIDs are not stable across it
- **Date:** 2026-09-15 (Phase 1)
- **Decision:** `HeapFile::Delete(rid)` works by scanning every live row, discarding the target, truncating the underlying file via `DiskManager::ResetFile`, and re-inserting every surviving row from scratch. This reassigns a new RID to every row that survives the delete, not just fills the gap left by the deleted one.
- **Alternative considered:** Tombstone the slot in place (mark it deleted, leave other slots' ids untouched) and reclaim space lazily via an explicit compaction pass later.
- **Why:** The project brief specifies exactly this baseline for Phase 1 — "append, scan, delete-by-rewrite" — as the deliberately trivial approach that Phase 2's B-tree then improves on. Since Phase 1 has no index yet, nothing depends on a RID surviving a Delete call, so the simplest correct implementation (throw everything away and rebuild) is the right choice for this phase specifically.
- **Consequence flagged for Phase 2:** this stops being safe the moment an index stores RIDs as index entries — a full-file rewrite on every delete would silently invalidate every index entry pointing at a surviving row. Phase 2 will need to switch to the tombstone-in-place alternative considered above (or something equivalent), which is a real, motivated design change, not a walk-back — it'll get its own decision entry when it happens.
- **Status:** active for Phase 1 only; expected to change in Phase 2, on purpose.

### D-011 — Buffer pool: no eviction, unbounded in-memory growth
- **Date:** 2026-09-15 (Phase 1)
- **Decision:** `BufferPool` caches every page it has ever touched for the process's lifetime, with no eviction policy at all.
- **Alternative considered:** A bounded cache with an eviction policy (LRU or clock), which is what any real database uses.
- **Why:** This is the brief's own explicit Phase 1 instruction ("start with the simplest possible cache policy — even 'no eviction, just grow' — to get correctness proven before optimizing"). It also happens to compose cleanly with D-004's no-steal durability design in Phase 3 (a page that's never evicted can't be written back to disk before its transaction commits by accident).
- **Status:** active. Revisit only if a later phase's benchmark (Phase 7) shows memory growth is an actual problem for the workloads being tested — not before, and not on suspicion alone.
