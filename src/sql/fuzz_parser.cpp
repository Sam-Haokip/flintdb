// A libFuzzer target for the lexer+parser (task #53, docs/DECISIONS.md
// D-053). Not part of flintdb_core, not linked into flintdb_tests or
// flintdb_cli -- built and run separately, by scripts/run_fuzz_parser.sh,
// because libFuzzer (-fsanitize=fuzzer) is a Clang-specific feature this
// project's main CMake build doesn't otherwise need.
//
// The question this asks is deliberately different from Phase 6's other
// oracle, the differential-testing harness (docs/DECISIONS.md D-051):
// that harness asks "does FlintDB's behavior match SQLite's, for SQL
// inside docs/SPEC.md's supported subset." This target asks something
// narrower and unrelated to SQLite entirely: "does Parse() ever crash,
// hang, or trip a sanitizer, for ANY byte sequence at all" -- including
// every kind of input the differential generator would never produce,
// because it stays deliberately inside the supported grammar (D-051's
// own words: "this is not a general 'throw arbitrary SQL at both
// engines' fuzzer"). A hand-written recursive-descent parser's actual
// failure modes -- unbounded recursion on deeply nested input, an
// off-by-one token index, an out-of-bounds read on a truncated or
// unexpected token stream -- are exactly the kind of thing malformed,
// not-even-trying-to-be-valid input finds, and exactly what this project
// has no other test coverage for: every existing parser test
// (tests/test_parser.cpp) feeds it SQL that's *supposed* to either parse
// correctly or fail with a specific, expected SqlSyntaxError -- none of
// them are adversarial in the way a fuzzer's corpus becomes over time.
//
// Only Parse() (parser.h) is exercised, not Tokenize() (lexer.h)
// separately: Parse() calls Tokenize() as its very first step
// (src/sql/parser.cpp), so every lexer-level crash is already reachable
// through this one entry point -- a second target would just be
// re-fuzzing the same code path from further out, for no added coverage.
#include "parser.h"
#include "sql_error.h"

#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // libFuzzer's native input model is already "arbitrary bytes, no
    // framing" -- unlike D-051's flintdb_cli, which had to invent a
    // length-prefixed protocol specifically to get arbitrary TEXT content
    // safely across a subprocess boundary, there's no boundary to cross
    // here at all: `data`/`size` go directly into a std::string (which
    // tolerates embedded NULs and invalid UTF-8 exactly as well as any
    // other byte sequence) and directly into Parse().
    std::string sql(reinterpret_cast<const char*>(data), size);

    try {
        flintdb::Parse(sql);
        // A successful parse is a fine outcome too -- this target isn't
        // checking the *result* is correct (the differential harness's
        // job, D-051), only that reaching one never crashes.
    } catch (const flintdb::SqlSyntaxError&) {
        // The expected, routine outcome for most inputs a fuzzer
        // generates: this is not malformed SQL, this is "not SQL," and
        // Parse()'s own contract (parser.h) is to reject it cleanly with
        // exactly this exception type.
    }
    // Deliberately not catching std::exception (or anything broader)
    // here: Parse()'s documented contract is to throw SqlSyntaxError and
    // nothing else for a shape it doesn't recognize (parser.h's own doc
    // comment). Any other exception type escaping this call -- or a
    // sanitizer error, or a crash, or a hang libFuzzer's own timeout
    // catches -- is exactly the kind of genuine bug this target exists
    // to surface, and letting it propagate uncaught is what makes
    // libFuzzer treat it as a finding to report and minimize, rather than
    // something silently swallowed here.
    return 0;
}
