#pragma once
//
// AnnPocketDb :: tests/test_common.hpp
//
// Minimal, dependency-free test harness (matches the project's own "zero
// external deps" kernel philosophy -- no GoogleTest/Catch2). Each test file
// has its own main(), uses CHECK/CHECK_OK to record failures, and returns
// the failure count (0 == success) so CTest sees a real pass/fail signal.
//
#include <cstdio>
#include <filesystem>
#include <string>

#include "annpocketdb/util/status.hpp"

namespace anntest {

inline int& failureCount() {
    static int n = 0;
    return n;
}

inline void reportFailure(const char* file, int line, const std::string& what) {
    std::fprintf(stderr, "[FAIL] %s:%d: %s\n", file, line, what.c_str());
    ++failureCount();
}

// Fresh, unique scratch path for a test's database file(s); caller-owned,
// removed at the start (not the end, so a failed run's file survives for
// inspection) of each CHECK-free helper call.
inline std::filesystem::path scratchDbPath(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path() / "annpocketdb_tests";
    std::filesystem::create_directories(dir);
    auto path = dir / name;
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    // Sidecars derived with path += suffix, never via path.string() (which
    // narrows through the ANSI code page on Windows -- see util/path.hpp).
    auto sidecar = [&](const char* suffix) {
        auto p = path;
        p += suffix;
        return p;
    };
    std::filesystem::remove_all(sidecar(".wal"), ec);
    std::filesystem::remove(sidecar(".lock"), ec);
    std::filesystem::remove(sidecar(".sha256"), ec);
    std::filesystem::remove(sidecar(".enc"), ec);
    return path;
}

}  // namespace anntest

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            anntest::reportFailure(__FILE__, __LINE__, "CHECK failed: " #cond); \
        }                                                                     \
    } while (0)

#define CHECK_OK(status_expr)                                                \
    do {                                                                     \
        auto _s = (status_expr);                                             \
        if (!_s.ok()) {                                                      \
            anntest::reportFailure(__FILE__, __LINE__,                       \
                                   std::string("expected OK, got: ") + _s.toString()); \
        }                                                                     \
    } while (0)

#define TEST_MAIN_END()                                                      \
    do {                                                                     \
        if (anntest::failureCount() == 0) {                                  \
            std::printf("[PASS] %s\n", __FILE__);                            \
        } else {                                                             \
            std::printf("[FAIL] %s (%d failure(s))\n", __FILE__, anntest::failureCount()); \
        }                                                                     \
        return anntest::failureCount();                                      \
    } while (0)
