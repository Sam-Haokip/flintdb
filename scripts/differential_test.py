#!/usr/bin/env python3
"""Phase 6 differential-testing harness (docs/DECISIONS.md D-051/D-052).

Generates random, seeded sequences of SQL restricted to docs/SPEC.md's
grammar, runs each sequence statement-by-statement against a real FlintDB
(via the flintdb_cli subprocess driver) and against real SQLite (Python's
stdlib sqlite3, configured to mirror FlintDB's NULL-free/autocommit
semantics), and asserts the two agree at every step: success/failure, and
-- for a SELECT -- the resulting row set compared as an unordered
multiset (row order is not a claim either engine makes without ORDER BY,
docs/SPEC.md section 1.3).

Usage:
    python3 scripts/differential_test.py [--seeds N] [--ops N] [--verbose]
    (after `cmake --build build --target flintdb_cli`)

Exit code is 0 iff every seed's entire sequence matched with no
discrepancies. On a mismatch, prints a full, seed-reproducible repro (the
statement sequence up to and including the first differing statement,
plus both engines' results for it) and continues to the next seed, so one
run surfaces every distinct mismatch rather than stopping at the first.
The final exit code is still non-zero if any seed found a mismatch.
"""
import argparse
import json
import random
import shutil
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path

DEFAULT_CLI = Path(__file__).resolve().parent.parent / "build" / "flintdb_cli"


# ---------------------------------------------------------------------
# Engine drivers. Both expose the same execute(sql) -> dict shape, so the
# generator and comparison logic below never need to know which engine
# they're looking at -- {"ok": True, "columns": [...], "rows": [[...],
# ...], "rows_affected": N} or {"ok": False, "error_type": "...",
# "error_message": "..."}.
# ---------------------------------------------------------------------


class FlintDBDriver:
    """Speaks flintdb_cli's length-prefixed input / JSON-Lines output
    protocol -- see docs/DECISIONS.md D-051 for why it's framed this way
    (a naive line-based protocol breaks on a TEXT value containing an
    embedded newline, which this generator can and does produce)."""

    def __init__(self, cli_path: Path, db_dir: str):
        self.proc = subprocess.Popen(
            [str(cli_path), db_dir],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def execute(self, sql: str) -> dict:
        data = sql.encode("utf-8")
        self.proc.stdin.write(f"{len(data)}\n".encode("ascii"))
        self.proc.stdin.write(data)
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            stderr = self.proc.stderr.read().decode("utf-8", "replace")
            raise RuntimeError(f"flintdb_cli exited unexpectedly on sql={sql!r}; stderr: {stderr}")
        return json.loads(line)

    def close(self):
        try:
            self.proc.stdin.close()
        except (BrokenPipeError, OSError):
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


class SqliteOracle:
    """Wraps Python's stdlib sqlite3 -- the real SQLite C library, not a
    reimplementation -- normalized to the same result shape flintdb_cli
    produces. isolation_level=None puts the connection in real autocommit
    mode (every statement commits immediately unless inside an explicit
    BEGIN), mirroring session.h's own autocommit default (D-049) rather
    than fighting Python's own legacy implicit-transaction heuristic."""

    def __init__(self):
        self.conn = sqlite3.connect(":memory:", isolation_level=None)

    def execute(self, sql: str) -> dict:
        try:
            cur = self.conn.execute(sql)
            if cur.description is not None:
                columns = [d[0] for d in cur.description]
                rows = [list(row) for row in cur.fetchall()]
                return {"ok": True, "columns": columns, "rows": rows, "rows_affected": 0}
            rows_affected = cur.rowcount if cur.rowcount and cur.rowcount >= 0 else 0
            return {"ok": True, "columns": [], "rows": [], "rows_affected": rows_affected}
        except sqlite3.Error as e:
            return {"ok": False, "error_type": type(e).__name__, "error_message": str(e)}

    def close(self):
        self.conn.close()


# ---------------------------------------------------------------------
# Comparison. See docs/DECISIONS.md D-051 for why each of these choices
# (multiset row comparison, success/failure not error text, exact
# rows_affected/columns) is the right target, decided before this
# generator was written.
# ---------------------------------------------------------------------


def _normalize_rows(rows):
    return sorted(tuple(row) for row in rows)


def results_match(flint_result: dict, sqlite_result: dict):
    """Returns (True, None) on agreement, else (False, reason)."""
    if flint_result["ok"] != sqlite_result["ok"]:
        return False, f"ok mismatch: flintdb={flint_result['ok']} sqlite={sqlite_result['ok']}"
    if not flint_result["ok"]:
        return True, None  # error text intentionally not compared (D-051)
    if flint_result["columns"] != sqlite_result["columns"]:
        return False, f"columns mismatch: flintdb={flint_result['columns']} sqlite={sqlite_result['columns']}"
    flint_rows = _normalize_rows(flint_result["rows"])
    sqlite_rows = _normalize_rows(sqlite_result["rows"])
    if flint_rows != sqlite_rows:
        return False, f"rows mismatch (order-independent): flintdb={flint_rows} sqlite={sqlite_rows}"
    if flint_result["rows_affected"] != sqlite_result["rows_affected"]:
        return False, (
            f"rows_affected mismatch: flintdb={flint_result['rows_affected']} "
            f"sqlite={sqlite_result['rows_affected']}"
        )
    return True, None


# ---------------------------------------------------------------------
# SQL generation, strictly within docs/SPEC.md's grammar (D-051: this is
# not a general "throw arbitrary SQL at both engines" fuzzer -- that's
# the separate parser-fuzzing work, D-052). Value domains are
# deliberately small (a handful of integers, a handful of strings) --
# standard practice for this kind of generator, since small domains
# maximize the odds of the interesting overlaps that actually exercise
# the engine (duplicate PRIMARY KEY values, WHERE predicates that match
# something, JOIN keys that line up) rather than every value being
# trivially unique.
# ---------------------------------------------------------------------

INT_DOMAIN = list(range(-5, 25))
TEXT_DOMAIN = [
    "alice", "bob", "carol", "",  # empty string -- a real, legal TEXT value
    "it's got a quote",  # exercises '' escaping
    "unicode: héllo wörld 中文",
    "  leading and trailing space  ",
]
COMPARATORS = ["=", "!=", "<", "<=", ">", ">="]


def sql_quote(text: str) -> str:
    """SQL string-literal escaping: double every embedded single quote.
    Identical convention in both engines (docs/DECISIONS.md D-044 for
    FlintDB's lexer; SQLite uses the same standard SQL convention), so
    one function serves both -- there is no backslash-escape special
    case in either engine's string literal grammar."""
    return "'" + text.replace("'", "''") + "'"


class Column:
    def __init__(self, name: str, is_integer: bool, is_pk: bool):
        self.name = name
        self.is_integer = is_integer
        self.is_pk = is_pk


class Table:
    def __init__(self, name: str, columns: list):
        self.name = name
        self.columns = columns

    def integer_columns(self):
        return [c for c in self.columns if c.is_integer]

    def text_columns(self):
        return [c for c in self.columns if not c.is_integer]


def random_literal_for(column: Column, rng: random.Random) -> str:
    if column.is_integer:
        return str(rng.choice(INT_DOMAIN))
    return sql_quote(rng.choice(TEXT_DOMAIN))


def generate_schema(rng: random.Random):
    """Builds 1-3 tables (2-4 columns each), an optional single-column
    INTEGER PRIMARY KEY per table, and optional secondary indexes on
    other INTEGER columns -- both PK and CREATE INDEX are restricted to
    INTEGER columns because the B+-tree has only ever supported int64_t
    keys (docs/DECISIONS.md D-046), which this generator respects by
    construction rather than generating a TEXT index and expecting
    Catalog to reject it identically on both sides (SQLite has no such
    restriction at all -- that would be manufacturing a known, already-
    documented scope difference, not a real comparison)."""
    tables = []
    ddl_statements = []
    num_tables = rng.choice([1, 2, 2, 3])  # bias toward 2 (enables JOIN generation)
    for t in range(num_tables):
        table_name = f"t{t}"
        num_cols = rng.randint(2, 4)
        columns = []
        for c in range(num_cols):
            is_integer = rng.random() < 0.6  # bias toward INTEGER: enables PK/index/join more often
            columns.append(Column(f"c{c}", is_integer, is_pk=False))

        pk_col = None
        int_cols = [c for c in columns if c.is_integer]
        if int_cols and rng.random() < 0.5:
            pk_col = rng.choice(int_cols)
            pk_col.is_pk = True

        col_defs = ", ".join(
            f"{c.name} {'INTEGER' if c.is_integer else 'TEXT'}" + (" PRIMARY KEY" if c.is_pk else "")
            for c in columns
        )
        ddl_statements.append((f"CREATE TABLE {table_name} ({col_defs})", table_name))

        for c in int_cols:
            if not c.is_pk and rng.random() < 0.4:
                ddl_statements.append((f"CREATE INDEX {table_name}_{c.name}_idx ON {table_name}({c.name})", table_name))

        tables.append(Table(table_name, columns))
    return tables, ddl_statements


def generate_column_ref(table: Table, column: Column, qualify: bool) -> str:
    return f"{table.name}.{column.name}" if qualify else column.name


def generate_where_clause(tables_in_scope, rng: random.Random, qualify: bool) -> str:
    num_predicates = rng.choice([1, 1, 2])
    predicates = []
    for _ in range(num_predicates):
        table = rng.choice(tables_in_scope)
        column = rng.choice(table.columns)
        comparator = rng.choice(COMPARATORS)
        literal = random_literal_for(column, rng)
        predicates.append(f"{generate_column_ref(table, column, qualify)} {comparator} {literal}")
    return "WHERE " + " AND ".join(predicates)


def generate_select(tables: list, rng: random.Random) -> str:
    join_candidates = []
    if len(tables) >= 2:
        for i, a in enumerate(tables):
            for b in tables[i + 1:]:
                for ca in a.columns:
                    for cb in b.columns:
                        if ca.is_integer == cb.is_integer:
                            join_candidates.append((a, ca, b, cb))

    if join_candidates and rng.random() < 0.3:
        outer, outer_col, inner, inner_col = rng.choice(join_candidates)
        select_list = "*" if rng.random() < 0.5 else ", ".join(
            generate_column_ref(t, c, qualify=True) for t in (outer, inner) for c in t.columns if rng.random() < 0.6
        ) or "*"
        sql = (
            f"SELECT {select_list} FROM {outer.name} JOIN {inner.name} "
            f"ON {outer.name}.{outer_col.name} = {inner.name}.{inner_col.name}"
        )
        if rng.random() < 0.5:
            sql += " " + generate_where_clause([outer, inner], rng, qualify=True)
        return sql

    table = rng.choice(tables)
    select_list = "*" if rng.random() < 0.6 else ", ".join(
        c.name for c in table.columns if rng.random() < 0.6
    ) or "*"
    sql = f"SELECT {select_list} FROM {table.name}"
    if rng.random() < 0.5:
        sql += " " + generate_where_clause([table], rng, qualify=False)
    return sql


def generate_insert(table: Table, rng: random.Random) -> str:
    values = ", ".join(random_literal_for(c, rng) for c in table.columns)
    return f"INSERT INTO {table.name} VALUES ({values})"


def generate_update(table: Table, rng: random.Random) -> str:
    num_assignments = rng.choice([1, 1, 2])
    columns = rng.sample(table.columns, min(num_assignments, len(table.columns)))
    assignments = ", ".join(f"{c.name} = {random_literal_for(c, rng)}" for c in columns)
    sql = f"UPDATE {table.name} SET {assignments}"
    if rng.random() < 0.6:
        sql += " " + generate_where_clause([table], rng, qualify=False)
    return sql


def generate_delete(table: Table, rng: random.Random) -> str:
    sql = f"DELETE FROM {table.name}"
    if rng.random() < 0.6:
        sql += " " + generate_where_clause([table], rng, qualify=False)
    return sql


class TxnState:
    """Tracks whether the generator currently believes a transaction is
    open, purely so it mostly generates well-formed BEGIN/COMMIT/ROLLBACK
    sequences -- with a small deliberate chance of generating a
    malformed one anyway (BEGIN while already open, COMMIT/ROLLBACK
    while not), specifically to compare that both engines reject it
    identically (docs/SPEC.md's transaction grammar, both raising a
    "not ok" result -- see docs/DECISIONS.md D-052 for why this
    generator doesn't otherwise try to model every transaction-failure
    interleaving: Phase 4/5's own unit tests already cover transaction
    semantics under error exhaustively; this harness's job is broad
    SQL-result agreement, not re-proving isolation/atomicity)."""

    def __init__(self):
        self.open = False


def generate_operation(tables, rng: random.Random, txn: TxnState) -> str:
    choices = ["insert"] * 4 + ["select"] * 4 + ["update"] * 2 + ["delete"] * 2
    if txn.open:
        choices += ["commit"] * 2 + ["rollback"] * 2 + ["begin"] * 1  # mostly close it out; rarely misuse
    else:
        choices += ["begin"] * 2 + ["commit"] * 1 + ["rollback"] * 1  # mostly open; rarely misuse

    kind = rng.choice(choices)
    if kind == "begin":
        txn.open = True
        return "BEGIN"
    if kind == "commit":
        txn.open = False
        return "COMMIT"
    if kind == "rollback":
        txn.open = False
        return "ROLLBACK"

    table = rng.choice(tables)
    if kind == "insert":
        return generate_insert(table, rng)
    if kind == "update":
        return generate_update(table, rng)
    if kind == "delete":
        return generate_delete(table, rng)
    return generate_select(tables, rng)


# ---------------------------------------------------------------------
# Driving one seed's whole sequence through both engines.
# ---------------------------------------------------------------------


def run_seed(seed: int, num_ops: int, cli_path: Path, verbose: bool):
    rng = random.Random(seed)
    tables, ddl_statements = generate_schema(rng)

    db_dir = tempfile.mkdtemp(prefix=f"flintdb_difftest_{seed}_")
    flint = FlintDBDriver(cli_path, db_dir)
    oracle = SqliteOracle()
    history = []

    def step(sql: str):
        history.append(sql)
        flint_result = flint.execute(sql)
        sqlite_result = oracle.execute(sql)
        ok, reason = results_match(flint_result, sqlite_result)
        if verbose:
            print(f"  [{seed}] {sql!r} -> flintdb={flint_result} sqlite={sqlite_result}")
        if not ok:
            return {
                "seed": seed,
                "reason": reason,
                "history": list(history),
                "flint_result": flint_result,
                "sqlite_result": sqlite_result,
            }
        return None

    try:
        for sql, _table_name in ddl_statements:
            mismatch = step(sql)
            if mismatch:
                return mismatch

        txn = TxnState()
        for _ in range(num_ops):
            sql = generate_operation(tables, rng, txn)
            mismatch = step(sql)
            if mismatch:
                return mismatch

            # Periodic full-state check: catches a divergence a narrowly
            # scoped generated statement's own result might miss (e.g.
            # an UPDATE that silently touched the wrong row but still
            # reported a plausible rows_affected).
            for table in tables:
                mismatch = step(f"SELECT * FROM {table.name}")
                if mismatch:
                    return mismatch

        # Leave no dangling explicit transaction for FlintDB's own D-006
        # push/finalize hygiene -- not required for correctness here, but
        # keeps each seed's flintdb_cli process exiting cleanly.
        if txn.open:
            step("ROLLBACK")

        return None
    finally:
        flint.close()
        oracle.close()
        shutil.rmtree(db_dir, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--seeds", type=int, default=50, help="number of independent random seeds to try")
    parser.add_argument("--ops", type=int, default=40, help="operations generated per seed")
    parser.add_argument("--start-seed", type=int, default=0)
    parser.add_argument("--flintdb-cli", type=Path, default=DEFAULT_CLI)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    if not args.flintdb_cli.exists():
        print(f"error: flintdb_cli not found at {args.flintdb_cli} -- build it first "
              f"(cmake --build build --target flintdb_cli)", file=sys.stderr)
        return 2

    mismatches = []
    for seed in range(args.start_seed, args.start_seed + args.seeds):
        mismatch = run_seed(seed, args.ops, args.flintdb_cli, args.verbose)
        if mismatch:
            mismatches.append(mismatch)
            print(f"MISMATCH at seed {seed}: {mismatch['reason']}")
            print(f"  repro (statement sequence, in order):")
            for i, sql in enumerate(mismatch["history"]):
                print(f"    {i}: {sql!r}")
            print(f"  flintdb: {mismatch['flint_result']}")
            print(f"  sqlite:  {mismatch['sqlite_result']}")
        else:
            print(f"seed {seed}: OK ({args.ops} ops)")

    print(f"\n{args.seeds - len(mismatches)}/{args.seeds} seeds matched with no discrepancies.")
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
