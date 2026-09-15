# FlintDB — Phase 0 Specification

**Status:** draft, Phase 0 — no code exists yet. This document is the contract for everything built in Phases 1–8. If an implementation detail contradicts this document, the document wins until it is deliberately revised (and the revision logged in `DECISIONS.md`).

FlintDB is a single-node, embedded, single-file relational database engine, written from scratch in C++. "Embedded" means it is a library linked into a host process, not a client-server system — there is no network protocol, no listener, no separate server process. Everything below is scoped tightly on purpose: the goal is a small system that is *fully* correct and *fully* understood, not a large one that's approximately right.

---

## 1. Supported SQL subset

Exactly six statement types are supported. Anything not listed here is out of scope for v1, not an oversight.

### 1.1 Data types

| Type | Storage | Notes |
|---|---|---|
| `INTEGER` | 8-byte signed (int64) | |
| `TEXT` | variable-length, UTF-8 | length-prefixed on disk |

No `REAL`/`FLOAT`, `BLOB`, `DATE`, `BOOLEAN`, or `NULL`-as-first-class-value handling beyond what's needed for `NOT NULL` enforcement. `NULL` values themselves are **not supported** in v1 — every column is implicitly `NOT NULL`. This is a real simplification (it removes three-valued logic from every WHERE/JOIN evaluation) and is called out explicitly rather than silently assumed.

### 1.2 Grammar (informal BNF)

```
create_table  ::= "CREATE" "TABLE" table_name "(" column_def ("," column_def)* ")"
column_def    ::= column_name type ["PRIMARY" "KEY"]
type          ::= "INTEGER" | "TEXT"

create_index  ::= "CREATE" "INDEX" index_name "ON" table_name "(" column_name ")"

insert        ::= "INSERT" "INTO" table_name "VALUES" "(" literal ("," literal)* ")"

select        ::= "SELECT" select_list "FROM" table_name [join_clause] [where_clause]
select_list   ::= "*" | column_ref ("," column_ref)*
join_clause   ::= "JOIN" table_name "ON" column_ref "=" column_ref
where_clause  ::= "WHERE" predicate ("AND" predicate)*
predicate     ::= column_ref comparator literal
comparator    ::= "=" | "!=" | "<" | "<=" | ">" | ">="
column_ref    ::= [table_name "."] column_name

update        ::= "UPDATE" table_name "SET" assignment ("," assignment)* [where_clause]
assignment    ::= column_name "=" literal

delete        ::= "DELETE" "FROM" table_name [where_clause]

literal       ::= integer_literal | string_literal
```

### 1.3 Explicit restrictions within supported statements

- `PRIMARY KEY` is single-column only; it is the table's clustering key (rows are physically ordered by it in the primary B+-tree). Exactly zero or one `PRIMARY KEY` per table. A table with no primary key is stored as an unordered heap file with no clustering index (Phase 1 baseline).
- `CREATE INDEX` builds one secondary B+-tree on exactly one column. Secondary indexes are non-unique (duplicate keys allowed) and store the primary key (or heap row-id, for PK-less tables) as the indexed value, not the full row.
- `JOIN` supports exactly one join, between exactly two tables, equi-join only (`=`), with an explicit `ON`. No implicit (comma-list) joins, no `LEFT`/`RIGHT`/`FULL OUTER`, no self-join restrictions beyond what the grammar naturally allows.
- `WHERE` supports a conjunction (`AND`) of simple comparisons. No `OR`, no `IN`, no `LIKE`, no `BETWEEN`, no parentheses/precedence — `AND`-only keeps the planner's job (does this predicate set match an index?) unambiguous, which matters for Phase 5's access-path decision.
- `SELECT` has no `ORDER BY`, `GROUP BY`, aggregate functions (`COUNT`, `SUM`, ...), `DISTINCT`, or `LIMIT`.
- `INSERT` requires a full value list in column-declaration order; no partial-column inserts, no `INSERT ... SELECT`.
- Transactions are `BEGIN` / `COMMIT` / `ROLLBACK` only — no savepoints, no nested transactions.

---

## 2. Durability guarantee

**Claim:** a transaction's effects are durable if and only if its `COMMIT` log record has been written and `fsync`'d to the WAL file. A crash before that fsync completes must leave the transaction's effects entirely absent after recovery — never partially applied.

**Recovery mechanism:** redo-only recovery under a **no-steal** buffer-pool policy.

- *No-steal* means a dirty page is never written back to the data file before the transaction that dirtied it has committed. In-memory-only changes for an uncommitted transaction can simply be discarded on abort — nothing on disk needs to be undone, because nothing on disk was ever touched.
- Because of no-steal, the WAL only needs **redo** records (physical or physiological "this page, this offset, becomes this value" records). On restart, the engine replays every WAL record for every transaction whose `COMMIT` record is present, in order, starting from the last checkpoint, and ignores records belonging to transactions with no `COMMIT` record.
- This is a deliberate simplification versus full ARIES-style recovery (steal/force policies with undo+redo logging and compensation log records). It is the standard trade: no-steal is simpler to implement and prove correct, at the cost of bounding transaction size by available buffer-pool memory (a transaction that dirties more pages than fit in the buffer pool cannot be supported under a strict no-steal policy). Given this project's scope, that bound is accepted.
- **This is a Phase 0 design target, not yet load-bearing engineering.** If Phase 3 finds no-steal unworkable (e.g., a test workload needs bigger transactions than the buffer pool), the policy moves to steal+undo and this section is revised, with the change and reasoning logged in `DECISIONS.md` — not silently.

**Explicitly not claimed:** point-in-time recovery, replication, backup/restore tooling, or any durability guarantee for data outside the single `.db` + `.wal` file pair.

---

## 3. Isolation guarantee

**Target isolation level: Serializable**, achieved via **Strict Two-Phase Locking (Strict 2PL)** — locks are only acquired during a transaction's growing phase and all locks are held until commit or abort (the "strict" part; this is what makes recovery and cascading-abort reasoning tractable, at the cost of holding locks longer than plain 2PL).

Chosen over MVCC for v1 specifically because 2PL's correctness argument is short enough to state and defend in an interview ("no transaction reads or writes anything another uncommitted transaction touched, because lock ownership makes that state unreachable"), whereas a correct MVCC implementation (snapshot management, garbage collection of old versions, write-skew handling to actually reach serializable rather than merely snapshot isolation) is a substantially larger project on its own. If time remains after Phase 4's 2PL implementation is tested and documented, MVCC is a stretch goal — added as an alternative, not a replacement, with both compared.

**Final answer (Phase 4, see `DECISIONS.md` D-021/D-022 for the full reasoning):**

- **Lock granularity: page-level**, not table-level. Table-level was rejected without needing a second full implementation to disprove it — it's analytically equivalent to at most one writer active on an entire table at a time, which isn't meaningfully "concurrency control" so much as a global mutex with extra steps. Page-level is empirically validated by a test that shows two transactions touching different pages of the same table both hold their locks and proceed without blocking (`lock_manager_page_level_grants_concurrent_access_to_different_pages_of_the_same_table`, `tests/test_lock_manager.cpp`) — something table-level locking could never allow.
- **Deadlock policy: wait-die** (prevention, not detection+timeout). Chosen specifically because it's provably deadlock-free by construction (a transaction only ever waits for an *older* transaction, so no wait-for cycle can ever form — no wait-for graph or periodic cycle detection needed) and, critically, only ever aborts the transaction *making the new request*, on its own thread, never a transaction actively running on another thread — which a preemptive scheme like wound-wait would require, and which is real added implementation risk this project doesn't need to take on.

**Anomalies this guarantee must prevent, each with a named test in Phase 4 (`tests/test_isolation.cpp`):**

| Anomaly | Prevented? |
|---|---|
| Dirty read | Yes — reader blocks on writer's exclusive lock (`isolation_dirty_read_is_prevented_the_reader_blocks_until_the_writer_commits`) |
| Lost update | Yes — writer holds exclusive lock through commit (`isolation_lost_update_is_prevented_by_exclusive_locks_held_through_commit`) |
| Non-repeatable read | Yes — reader's shared lock held until commit, i.e. strictness (`isolation_non_repeatable_read_is_prevented_because_the_writer_waits_out_the_whole_reader_transaction`) |
| Phantom read | **Partially — a documented, tested gap, not an oversight — and narrower than it first looks.** `HeapFile::Insert()` always tries to exclusively lock whatever page is currently *last* before ever checking whether it has room, which turns out to have a side effect worth calling out: a full-table `Scan()` locks every existing page, including that last one, so it actually blocks *any* concurrent insert for its whole duration — even one that would ultimately need to allocate a brand-new page (`isolation_phantom_read_is_prevented_when_a_full_scan_has_already_locked_every_existing_page`). An earlier draft of this row described the gap as "a phantom on a brand-new page after a scan," on the assumption that case was the open hole; it isn't — that scenario was tried and disproven before this row was written (see D-023). The real, demonstrated gap is `HeapFile::Find()`'s early return: it stops and locks only as far as the first matching page, so any page after that point is never locked, and a concurrent insert can land there — including a brand-new page — without blocking at all, and a later `Find()`/`Scan()` in the same still-open transaction can then observe it (`isolation_phantom_read_gap_a_find_that_returns_early_never_locks_the_pages_it_never_reached`). Closing this would need a coarser lock for range predicates (e.g. a table-level intent lock held for the duration of a scan) that Phase 4 deliberately doesn't add, since it would mean every scan blocks every insert into the same table regardless of overlap, at exactly the granularity page-level locking was chosen to avoid. See D-022/D-023 for the full reasoning. |

---

## 4. Non-goals (explicit)

Stated here so nobody — including future us — mistakes an absence for an oversight:

- No distributed transactions, replication, clustering, or any multi-node behavior. FlintDB is single-process, single-file, single-node, by design.
- No cost-based query optimizer. The planner is a fixed rule: use an index scan if the `WHERE` clause contains an equality (or, if implemented, range) predicate on an indexed column, else sequential scan. No statistics, no cardinality estimation, no join reordering.
- No subqueries, window functions, `GROUP BY`/aggregates, `ORDER BY`, `OR` predicates, `IN`/`LIKE`/`BETWEEN`, multi-way (>2 table) joins, or outer joins.
- No `ALTER TABLE` or any schema migration; no concurrent DDL (schema changes require exclusive access to the whole database).
- No foreign keys or referential-integrity enforcement across tables.
- No `NULL` value support (see §1.1).
- No client-server networking, connection pooling, or authentication — embedded only.
- No general crash-safety for the *host process* misusing the API (e.g., calling into the engine from multiple threads without going through its own transaction/locking API is undefined behavior, not a supported concurrent-use pattern).

---

## 5. Verification standard

No claim in this project counts until it's measured or tested:

- Every performance claim (index vs. scan, before/after a benchmark fix) is a repeated-runs measurement with variance reported, not a single run.
- Every durability claim is backed by an actual simulated-crash test at a specific point in the write path, not "the WAL code looks right."
- Every isolation claim is backed by a named test reproducing (or failing to reproduce) the specific anomaly, not a general "transactions work."
- Every correctness claim about SQL semantics is checked differentially against real SQLite (Phase 6), not just by inspection.

---

## 6. Open questions carried into later phases

These are known-unresolved on purpose — Phase 0 states the question precisely; the relevant phase answers it with a measurement or a tested design, and the answer gets logged in `DECISIONS.md`:

1. Lock granularity for Strict 2PL (table vs. page-level) — Phase 4.
2. Deadlock handling: detection+timeout vs. prevention (wait-die/wound-wait) — Phase 4.
3. Whether no-steal survives contact with a real benchmark workload, or needs to become steal+undo — Phase 3/7.
4. B+-tree fanout / page size — Phase 2, chosen and justified with the lookup-time benchmark, not guessed.
5. Whether `REAL` and range-predicate index scans (`<`, `>`, `<=`, `>=` against an index, not just `=`) are worth adding as stretch scope once the v1 core is done and tested — end of Phase 5 at the earliest, never before.
