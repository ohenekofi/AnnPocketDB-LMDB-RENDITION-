//
// AnnPocketDb :: tests/test_mplay_soak.cpp
//
// Cross-process counterpart to test_soak_concurrent.cpp. test_mplay_fuzzer
// already proves the multi-process reader-table path is correct for a
// short, fixed run; this variant runs the same mplay_worker binary as real
// OS processes for a configurable wall-clock duration, with configurable
// process counts, so the *cross-process* lock-file / shared-memory reader
// table (platform/interprocess_lock.hpp) gets the same sustained scheduling
// pressure the in-process soak test gives the in-process paths.
//
// This is the one class of bug a single-process TSan run structurally
// cannot see: TSan instruments one process's memory space. Cross-process
// shared-memory races (the reader slot table mapped MAP_SHARED / via a
// Windows file mapping) need either (a) sustained real multi-process load
// plus careful manual reasoning / a model, or (b) running each spawned
// worker itself under TSan and relying on volume + halt_on_error to surface
// anything -- which is exactly what this test enables when built under
// -DANNPOCKETDB_SANITIZE=thread, since mplay_worker gets instrumented too
// (see cmake/Sanitizers.cmake).
//
// Duration: ANNPOCKETDB_SOAK_SECONDS (default 5). Process counts:
// ANNPOCKETDB_SOAK_WRITERS / ANNPOCKETDB_SOAK_READERS (default 3 each).
// Each "round" spawns a fresh batch of worker processes with a modest
// iteration count and waits for them; rounds repeat until the deadline.
// Spawning fresh processes per round (rather than one long-lived batch) is
// deliberate -- it also exercises reader-slot *registration/deregistration*
// churn under contention, not just steady-state reads, which is where the
// disclosed "no TSan/soak" gap is most likely to bite.
//
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "process_spawn.hpp"
#include "test_common.hpp"

#ifndef MPLAY_WORKER_PATH
#error "MPLAY_WORKER_PATH must be defined by CMake (see tests/CMakeLists.txt)"
#endif

namespace {

constexpr int kIterationsPerRound = 200;

}  // namespace

int main() {
    int soakSeconds = 5;
    if (const char* envSeconds = std::getenv("ANNPOCKETDB_SOAK_SECONDS")) {
        int parsed = std::atoi(envSeconds);
        if (parsed > 0) soakSeconds = parsed;
    }
    int writerCount = 3;
    if (const char* envW = std::getenv("ANNPOCKETDB_SOAK_WRITERS")) {
        int parsed = std::atoi(envW);
        if (parsed > 0) writerCount = parsed;
    }
    int readerCount = 3;
    if (const char* envR = std::getenv("ANNPOCKETDB_SOAK_READERS")) {
        int parsed = std::atoi(envR);
        if (parsed > 0) readerCount = parsed;
    }

    auto dbPath = anntest::scratchDbPath("soak_mplay.apdb");
    std::string dbPathStr = dbPath.string();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(soakSeconds);
    int round = 0;
    int totalFailures = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        ++round;
        std::vector<mplay::ChildProcess> children;
        std::vector<std::string> roles;  // parallel to children, for failure reporting

        for (int i = 0; i < writerCount; ++i) {
            mplay::ChildProcess c;
            unsigned seed = static_cast<unsigned>(round * 1000 + i);
            bool spawned = mplay::spawnProcess(
                MPLAY_WORKER_PATH,
                {dbPathStr, "writer", std::to_string(kIterationsPerRound), std::to_string(seed)}, c);
            CHECK(spawned);
            if (spawned) {
                children.push_back(c);
                roles.push_back("writer#" + std::to_string(i));
            }
        }
        for (int i = 0; i < readerCount; ++i) {
            mplay::ChildProcess c;
            unsigned seed = static_cast<unsigned>(round * 1000 + 500 + i);
            bool spawned = mplay::spawnProcess(
                MPLAY_WORKER_PATH,
                {dbPathStr, "reader", std::to_string(kIterationsPerRound), std::to_string(seed)}, c);
            CHECK(spawned);
            if (spawned) {
                children.push_back(c);
                roles.push_back("reader#" + std::to_string(i));
            }
        }

        for (std::size_t i = 0; i < children.size(); ++i) {
            int code = mplay::waitForExit(children[i]);
            if (code != 0) {
                ++totalFailures;
                std::fprintf(stderr,
                             "[SOAK-FAIL] round %d: %s exited with code %d "
                             "(torn read / lock contention bug -- see stderr above from the worker)\n",
                             round, roles[i].c_str(), code);
            }
        }
    }

    CHECK(totalFailures == 0);
    std::printf("[SOAK] test_mplay_soak: %d round(s) over %d second(s), %d process failure(s)\n",
                round, soakSeconds, totalFailures);

    TEST_MAIN_END();
}
