// flintdb_cli -- a small subprocess driver used by Phase 6's differential
// testing harness (scripts/differential_test.py) and by nothing else.
// FlintDB itself is an embedded library with no CLI/server of its own
// (docs/SPEC.md's "embedded" framing) -- this tool exists purely so an
// external Python process can submit SQL to a real Database and observe
// results, the same way it can already talk to SQLite through Python's
// stdlib sqlite3 module. See docs/DECISIONS.md D-051 for the full design
// reasoning behind the protocol below.
//
// Usage: flintdb_cli <database-directory>
//
// Protocol (deliberately not a line-based text format -- see D-051 for
// why a naive one breaks on TEXT values containing embedded newlines):
//
//   Input (stdin), one statement at a time:
//     <decimal byte length>\n
//     <exactly that many raw bytes -- the SQL statement text, which may
//      contain any byte value at all, including newlines and NUL>
//   No delimiter follows the raw bytes -- the next thing on the stream is
//   immediately the next statement's length line (or EOF).
//
//   Output (stdout), one JSON object per line, in input order, flushed
//   immediately after each so a synchronous request/response caller never
//   blocks waiting on buffered output:
//     success: {"ok":true,"columns":[...],"rows":[[...],...],"rows_affected":N}
//     failure: {"ok":false,"error_type":"...","error_message":"..."}
//
//   This tool only ever *encodes* JSON, never parses it (it never
//   receives any) -- see JsonEncodeString below for why that asymmetry
//   means a hand-written encoder is the right amount of code here, not an
//   external JSON library.
#include "../src/db/database.h"
#include "../src/sql/parser.h"
#include "../src/sql/session.h"
#include "../src/sql/sql_error.h"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace flintdb;

namespace {

// Appends `text`'s JSON-escaped form (with surrounding quotes) to `out`.
// Handles exactly what JSON's grammar requires: '"', '\\', and every
// control character (0x00-0x1F) via the standard short escapes where one
// exists (\n \t \r \b \f) or \u00XX otherwise. Everything else --
// including every byte of a multi-byte UTF-8 sequence -- passes through
// unchanged: FlintDB's TEXT values are UTF-8 (docs/SPEC.md section 1.1),
// and valid UTF-8 bytes above 0x7F never collide with any ASCII
// character JSON requires escaping, so no decoding step is needed to
// re-encode them correctly.
void JsonEncodeString(const std::string& text, std::string* out) {
    out->push_back('"');
    for (unsigned char c : text) {
        switch (c) {
            case '"':
                out->append("\\\"");
                break;
            case '\\':
                out->append("\\\\");
                break;
            case '\n':
                out->append("\\n");
                break;
            case '\t':
                out->append("\\t");
                break;
            case '\r':
                out->append("\\r");
                break;
            case '\b':
                out->append("\\b");
                break;
            case '\f':
                out->append("\\f");
                break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                    out->append(buf);
                } else {
                    out->push_back(static_cast<char>(c));
                }
        }
    }
    out->push_back('"');
}

// Encodes one LiteralValue as a bare JSON value (a number for INTEGER, a
// quoted/escaped string for TEXT) -- printed in plain decimal with no
// decimal point or exponent so Python's json module parses it as an
// exact (arbitrary-precision) int, never a lossy float.
void JsonEncodeLiteral(const LiteralValue& value, std::string* out) {
    if (value.kind == LiteralValue::Kind::kInteger) {
        out->append(std::to_string(value.int_value));
    } else {
        JsonEncodeString(value.string_value, out);
    }
}

std::string EncodeSuccess(const ExecuteResult& result) {
    std::string out = "{\"ok\":true,\"columns\":[";
    for (size_t i = 0; i < result.column_names.size(); i++) {
        if (i > 0) out.push_back(',');
        JsonEncodeString(result.column_names[i], &out);
    }
    out.append("],\"rows\":[");
    for (size_t r = 0; r < result.rows.size(); r++) {
        if (r > 0) out.push_back(',');
        out.push_back('[');
        const auto& row = result.rows[r];
        for (size_t c = 0; c < row.size(); c++) {
            if (c > 0) out.push_back(',');
            JsonEncodeLiteral(row[c], &out);
        }
        out.push_back(']');
    }
    out.append("],\"rows_affected\":");
    out.append(std::to_string(result.rows_affected));
    out.push_back('}');
    return out;
}

// Classifies a caught exception by its concrete type, mirroring exactly
// the set session.h's Execute (and everything it calls into) can throw --
// see sql_error.h, catalog.h, and session.h's own doc comment for the
// full list this switches on. The Python harness compares FlintDB and
// SQLite on success/failure, not exact error text (D-051), but the type
// tag still lets it tell a syntax error from a logic error without
// string-matching the message, which matters for triaging what a
// mismatch actually means.
std::string EncodeFailure(const std::string& error_type, const std::string& message) {
    std::string out = "{\"ok\":false,\"error_type\":";
    JsonEncodeString(error_type, &out);
    out.append(",\"error_message\":");
    JsonEncodeString(message, &out);
    out.push_back('}');
    return out;
}

// Reads exactly `length` raw bytes from stdin. Throws std::runtime_error
// on a short read (a malformed or truncated protocol stream -- a driver
// bug, not something a caller should see as a SQL result).
std::string ReadExactBytes(size_t length) {
    std::string buf(length, '\0');
    if (length > 0) {
        std::cin.read(&buf[0], static_cast<std::streamsize>(length));
        if (static_cast<size_t>(std::cin.gcount()) != length) {
            throw std::runtime_error("flintdb_cli: truncated input stream (expected " + std::to_string(length) +
                                      " bytes, got " + std::to_string(std::cin.gcount()) + ")");
        }
    }
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <database-directory>\n";
        return 2;
    }

    Database db(argv[1]);

    std::string length_line;
    while (std::getline(std::cin, length_line)) {
        if (length_line.empty()) continue;  // tolerate a stray blank line, e.g. trailing newline at EOF
        size_t length;
        try {
            length = static_cast<size_t>(std::stoull(length_line));
        } catch (const std::exception&) {
            std::cerr << "flintdb_cli: malformed length line: " << length_line << "\n";
            return 2;
        }

        std::string sql_text = ReadExactBytes(length);
        std::string line;
        try {
            Statement stmt = Parse(sql_text);
            ExecuteResult result = Execute(db, stmt);
            line = EncodeSuccess(result);
        } catch (const SqlSyntaxError& e) {
            line = EncodeFailure("SqlSyntaxError", e.what());
        } catch (const SqlSemanticError& e) {
            line = EncodeFailure("SqlSemanticError", e.what());
        } catch (const std::invalid_argument& e) {
            // Must be caught before std::logic_error -- it derives from it.
            line = EncodeFailure("std::invalid_argument", e.what());
        } catch (const std::out_of_range& e) {
            // Must be caught before std::logic_error -- it derives from it.
            line = EncodeFailure("std::out_of_range", e.what());
        } catch (const std::logic_error& e) {
            line = EncodeFailure("std::logic_error", e.what());
        } catch (const std::runtime_error& e) {
            line = EncodeFailure("std::runtime_error", e.what());
        } catch (const std::exception& e) {
            line = EncodeFailure("std::exception", e.what());
        }

        std::cout << line << "\n";
        std::cout.flush();
    }

    return 0;
}
