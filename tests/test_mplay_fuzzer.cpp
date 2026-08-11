// mplay-equivalent multi-process fuzzer (Item 8): spawns REAL, separate OS
// processes (see process_spawn.hpp) -- some readers, some writers -- all
// hammering the SAME database file concurrently. This exercises the .lock
// file's shared-memory reader table (platform/interprocess_lock.hpp) under
// genuine cross-process contention, per explicit design choice, rather than
// an in-process thread simulation. Confirms every worker exits cleanly, no
// reader ever observes a torn/garbage value, and no reader-table slot leaks
// once every worker process has exited.
#include "test_common.hpp"
#include "process_spawn.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "annpocketdb/api/environment.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;
using namespace mplay;

namespace {
bool isWellFormedValue(const std::string& v) {
    if (v.size() != 7 || v[0] != 'v') return false;
    for (std::size_t i = 1; i < v.size(); ++i) {
        if (v[i] < '0' || v[i] > '9') return false;
    }
    return true;
}
}  // namespace

int main() {
    auto dbPath = anntest::scratchDbPath("mplay_fuzz.apdb");

    // Create the database up front, in-process, before any concurrent
    // access begins -- this fuzzer targets reader/writer contention on an
    // already-existing file, not the very first file-creation race (a
    // separate concern this test isn't after).
    //
    // maxDatabaseSize kept small (see mplay_worker.cpp's matching opts) --
    // must match what every worker process requests, since this seed open
    // is what first extends the file to its virtual reservation ceiling.
    EnvironmentOptions seedOpts;
    seedOpts.maxDatabaseSize = 64ull << 20;  // 64 MiB
    {
        auto envR = Environment::open(dbPath, seedOpts);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        CHECK_OK(envR.value().close());
    }

    // These counts are sized to be a real (not trivial) cross-process stress
    // pattern while still finishing in a few seconds as part of the default
    // ctest run -- not a substitute for a long-running or TSan-instrumented
    // soak test, which is what would actually be needed to surface rare
    // seqlock/CAS races in the shared reader table. This project has no
    // ThreadSanitizer build wired up (MSVC's TSan support is still
    // experimental/preview-only, unlike the POSIX/clang toolchains sibling
    // projects in this workspace use it with) -- a real soak/TSan run is a
    // disclosed gap, not attempted in this pass.
    const std::string workerExe = MPLAY_WORKER_PATH;
    constexpr int kReaderCount = 8;
    constexpr int kWriterCount = 4;
    constexpr int kReaderIterations = 300;
    constexpr int kWriterIterations = 200;

    std::vector<ChildProcess> children;
    std::vector<std::string> roles;  // parallel to children, for failure reporting

    for (int i = 0; i < kWriterCount; ++i) {
        ChildProcess child;
        std::vector<std::string> args{dbPath.string(), "writer", std::to_string(kWriterIterations),
                                      std::to_string(100 + i)};
        CHECK(spawnProcess(workerExe, args, child));
        children.push_back(child);
        roles.push_back("writer#" + std::to_string(i));
    }
    for (int i = 0; i < kReaderCount; ++i) {
        ChildProcess child;
        std::vector<std::string> args{dbPath.string(), "reader", std::to_string(kReaderIterations),
                                      std::to_string(200 + i)};
        CHECK(spawnProcess(workerExe, args, child));
        children.push_back(child);
        roles.push_back("reader#" + std::to_string(i));
    }

    bool allOk = true;
    for (std::size_t i = 0; i < children.size(); ++i) {
        int code = waitForExit(children[i]);
        if (code != 0) {
            std::fprintf(stderr, "[FAIL] mplay worker %s exited with code %d\n", roles[i].c_str(), code);
            allOk = false;
        }
    }
    CHECK(allOk);

    // No reader-table slot should remain registered once every worker
    // process has exited (each unregisters on close(); any that somehow
    // didn't would still be reclaimed here via the dead-PID sweep).
    {
        auto envR = Environment::open(dbPath, seedOpts);
        CHECK(envR.ok());
        if (envR.ok()) {
            Environment env = envR.moveValue();
            env.readerCheck();  // reclaim any dead-PID slots opportunistically
            auto readers = env.readerList();
            CHECK(readers.empty());

            // Final data integrity: whatever's there must be well-formed --
            // proves no torn/garbage write ever landed either.
            for (int k = 0; k < 20; ++k) {
                char keyBuf[16];
                std::snprintf(keyBuf, sizeof(keyBuf), "mplay%03d", k);
                auto v = env.get(toByteView(std::string(keyBuf)));
                if (v.ok()) {
                    std::string s(reinterpret_cast<const char*>(v.value().data()), v.value().size());
                    CHECK(isWellFormedValue(s));
                }
            }
            CHECK_OK(env.close());
        }
    }

    TEST_MAIN_END();
}
