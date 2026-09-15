#pragma once
// A deliberately tiny, in-house test harness instead of pulling in
// GoogleTest/Catch2 — see docs/DECISIONS.md for why. Self-registering
// FLINTDB_TEST blocks, two assertion macros, one runner. That's the whole
// feature set this project needs.

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace flintdb::testing {

struct TestFailure {
    std::string message;
};

inline void Fail(const std::string& msg, const char* file, int line) {
    std::ostringstream oss;
    oss << msg << " (" << file << ":" << line << ")";
    throw TestFailure{oss.str()};
}

using TestFn = std::function<void()>;

struct TestCase {
    std::string name;
    TestFn fn;
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> registry;
    return registry;
}

struct Registrar {
    Registrar(const std::string& name, TestFn fn) { Registry().push_back({name, std::move(fn)}); }
};

inline int RunAll() {
    int failed = 0;
    for (auto& tc : Registry()) {
        try {
            tc.fn();
            std::cout << "[ OK ] " << tc.name << "\n";
        } catch (const TestFailure& f) {
            std::cout << "[FAIL] " << tc.name << ": " << f.message << "\n";
            failed++;
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << tc.name << ": uncaught exception: " << e.what() << "\n";
            failed++;
        }
    }
    std::cout << (Registry().size() - failed) << "/" << Registry().size() << " tests passed\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace flintdb::testing

#define FLINTDB_TEST(name)                                                                 \
    void name();                                                                            \
    static flintdb::testing::Registrar registrar_##name(#name, name);                       \
    void name()

#define FLINTDB_CHECK(cond)                                                                 \
    do {                                                                                     \
        if (!(cond)) flintdb::testing::Fail("CHECK failed: " #cond, __FILE__, __LINE__);     \
    } while (0)

#define FLINTDB_CHECK_EQ(a, b)                                                              \
    do {                                                                                     \
        auto _flintdb_a = (a);                                                               \
        auto _flintdb_b = (b);                                                               \
        if (!(_flintdb_a == _flintdb_b)) {                                                   \
            std::ostringstream _flintdb_oss;                                                 \
            _flintdb_oss << "CHECK_EQ failed: " #a " != " #b " (" << _flintdb_a << " vs "    \
                          << _flintdb_b << ")";                                              \
            flintdb::testing::Fail(_flintdb_oss.str(), __FILE__, __LINE__);                  \
        }                                                                                     \
    } while (0)
