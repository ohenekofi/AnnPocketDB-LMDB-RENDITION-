// Crash-consistency fuzzing: this project's own review identified zero
// automated coverage of crash recovery (no test ever references wal::Wal,
// and backup::repair() -- a real, shipped, CLI-exposed function -- was never
// exercised). Three tiers:
//
//   Tier 1 (static corruption): truncate/bit-flip a valid closed .apdb file
//     at random offsets; assert open()/verify()/repair() never crash/hang,
//     whatever else they report.
//   Tier 2 (random-timing process kill): spawn a real worker process
//     committing sequential keys, hard-kill it after a random short delay,
//     reopen and check the "prefix property" (see checkPrefixProperty
//     below) plus backup::verify().
//   Tier 3 (precise crash-window targeting): drive crash_worker's
//     precommit/prepared/postmetaflip scenarios, each landing deterministically
//     on one specific named hazard window from PageManager::commit()'s own
//     comments, and assert the exact expected outcome (not just "didn't
//     crash").
//
// The core correctness oracle throughout is the "prefix property": a single
// writer commits strictly sequentially numbered keys, so after any crash,
// recovery must land on *some prefix* of that sequence -- keys [0,k) all
// present and correct, keys [k,N) all absent. A "hole" (key 5 missing but
// key 7 present) or a torn value is the actual bug this suite exists to
// catch; outright data loss of the most recent, never-durably-confirmed
// write is expected and not a failure.
#include "test_common.hpp"
#include "process_spawn.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/backup/maintenance.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;
using namespace mplay;

namespace {

std::string keyFor(long long i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "ck%06lld", i);
    return std::string(buf);
}
std::string valFor(long long i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "val%06lld", i);
    return std::string(buf);
}

// Scans keys 0..scanLimit against `env`. Returns the length of the clean
// leading prefix (largest k such that keys [0,k) are present with the exact
// expected value) and CHECKs that no key beyond that point is present
// (no holes, no resurrected data) -- the central assertion of this suite.
long long checkPrefixProperty(Environment& env, long long scanLimit) {
    long long prefixEnd = -1;
    for (long long i = 0; i < scanLimit; ++i) {
        auto v = env.get(toByteView(keyFor(i)));
        bool present = v.ok();
        if (prefixEnd < 0) {
            if (!present) {
                prefixEnd = i;
            } else {
                CHECK(toStringView(toByteView(v.value())) == valFor(i));
            }
        } else {
            CHECK(!present);  // past the prefix end -- must stay absent
        }
    }
    return prefixEnd < 0 ? scanLimit : prefixEnd;
}

}  // namespace

int main() {
    const std::string workerExe = CRASH_WORKER_PATH;

    // -------------------------------------------------------------------
    // Tier 1: static corruption fuzzing (no process spawning)
    // -------------------------------------------------------------------
    {
        // Small maxDatabaseSize: the file must be small enough to read
        // wholesale (default 1 TiB is sparse-pre-extended -- see the
        // fileContainsBytes fix elsewhere in this suite for the exact same
        // gotcha with a naive whole-file read).
        constexpr std::uint64_t kSmallMaxDb = 2ull << 20;  // 2 MiB
        auto basePath = anntest::scratchDbPath("crash_tier1_base.apdb");
        {
            EnvironmentOptions opts;
            opts.maxDatabaseSize = kSmallMaxDb;
            auto envR = Environment::open(basePath, opts);
            CHECK(envR.ok());
            if (envR.ok()) {
                Environment env = envR.moveValue();
                for (int i = 0; i < 500; ++i) {
                    CHECK_OK(env.put(toByteView(keyFor(i)), toByteView(valFor(i))));
                }
                CHECK_OK(env.close());
            }
        }
        std::vector<char> original;
        {
            std::ifstream in(basePath, std::ios::binary);
            original.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        CHECK(!original.empty());

        std::mt19937 rng(0xC0A5Au);
        std::uniform_int_distribution<int> modeDist(0, 1);  // 0 = truncate, 1 = corrupt
        constexpr int kTrials = 200;
        for (int trial = 0; trial < kTrials; ++trial) {
            std::vector<char> mutated = original;
            std::uniform_int_distribution<std::size_t> offDist(1, mutated.size() - 1);
            if (modeDist(rng) == 0) {
                mutated.resize(offDist(rng));
            } else {
                std::size_t off = offDist(rng);
                std::size_t len = std::min<std::size_t>(8, mutated.size() - off);
                for (std::size_t k = 0; k < len; ++k) {
                    mutated[off + k] = static_cast<char>(~mutated[off + k]);
                }
            }

            auto trialPath = anntest::scratchDbPath("crash_tier1_trial.apdb");
            {
                std::ofstream out(trialPath, std::ios::binary | std::ios::trunc);
                out.write(mutated.data(), static_cast<std::streamsize>(mutated.size()));
            }

            // Never crash/hang, whatever else happens.
            EnvironmentOptions opts;
            opts.maxDatabaseSize = kSmallMaxDb;
            auto envR = Environment::open(trialPath, opts);
            if (envR.ok()) {
                Environment env = envR.moveValue();
                (void)backup::verify(env);  // issues expected/fine; must not hang
                (void)env.close();
            }
            auto repairedPath = anntest::scratchDbPath("crash_tier1_repaired.apdb");
            backup::RepairOptions ropts;
            ropts.forceOverwrite = true;
            (void)backup::repair(trialPath, repairedPath, ropts);  // must not hang/crash

            std::error_code ec;
            std::filesystem::remove(trialPath, ec);
            std::filesystem::remove(repairedPath, ec);
            std::filesystem::remove(std::filesystem::path(repairedPath).string() + ".lock", ec);
        }
    }

    // -------------------------------------------------------------------
    // Tier 2: random-timing process-kill fuzzing (with and without WAL)
    // -------------------------------------------------------------------
    for (int walFlag = 0; walFlag <= 1; ++walFlag) {
        constexpr std::uint64_t kSmallMaxDb = 8ull << 20;  // 8 MiB
        auto path = anntest::scratchDbPath(walFlag ? "crash_tier2_wal.apdb" : "crash_tier2_nowal.apdb");
        {
            EnvironmentOptions seedOpts;
            seedOpts.maxDatabaseSize = kSmallMaxDb;
            seedOpts.enableWal = (walFlag != 0);
            auto envR = Environment::open(path, seedOpts);
            CHECK(envR.ok());
            if (!envR) continue;
            CHECK_OK(envR.value().close());
        }

        std::mt19937 rng(0xB17E5u + static_cast<unsigned>(walFlag));
        std::uniform_int_distribution<int> delayMs(0, 15);
        long long nextIndex = 0;
        constexpr int kRounds = 15;
        for (int round = 0; round < kRounds; ++round) {
            ChildProcess child;
            std::vector<std::string> args{path.string(), "loop", walFlag ? "1" : "0",
                                          std::to_string(nextIndex), "0"};
            CHECK(spawnProcess(workerExe, args, child));
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs(rng)));
            killProcess(child);
            waitForExit(child);

            long long confirmed = nextIndex - 1;  // conservative floor if no progress recorded
            {
                std::ifstream pf(path.string() + ".progress");
                long long v = 0;
                if (pf && (pf >> v)) confirmed = v;
            }

            EnvironmentOptions opts;
            opts.maxDatabaseSize = kSmallMaxDb;
            opts.enableWal = (walFlag != 0);
            auto envR = Environment::open(path, opts);
            CHECK(envR.ok());
            if (!envR) break;
            Environment env = envR.moveValue();

            auto vr = backup::verify(env);
            CHECK(vr.ok());
            if (vr.ok()) CHECK(vr.value().ok());

            long long prefixLen = checkPrefixProperty(env, confirmed + 3000);
            if (walFlag) {
                // Durable + WAL: nothing the worker itself saw as
                // successfully committed may ever be lost.
                CHECK(prefixLen >= confirmed + 1);
            }
            nextIndex = prefixLen;
            CHECK_OK(env.close());
        }
    }

    // -------------------------------------------------------------------
    // Tier 3: precise crash-window targeting
    // -------------------------------------------------------------------
    {
        auto runScenario = [&](const std::string& scenario, bool wal, long long confirmCount,
                               bool expectFinalPresent) {
            constexpr std::uint64_t kSmallMaxDb = 4ull << 20;  // 4 MiB
            auto path = anntest::scratchDbPath("crash_tier3_" + scenario + (wal ? "_wal" : "_nowal") + ".apdb");
            {
                EnvironmentOptions seedOpts;
                seedOpts.maxDatabaseSize = kSmallMaxDb;
                seedOpts.enableWal = wal;
                auto envR = Environment::open(path, seedOpts);
                CHECK(envR.ok());
                if (!envR) return;
                CHECK_OK(envR.value().close());
            }

            ChildProcess child;
            std::vector<std::string> args{path.string(), scenario, wal ? "1" : "0", "0",
                                          std::to_string(confirmCount)};
            CHECK(spawnProcess(workerExe, args, child));
            int code = waitForExit(child);
            CHECK(code != 0);  // must have crashed, never a clean exit

            EnvironmentOptions opts;
            opts.maxDatabaseSize = kSmallMaxDb;
            opts.enableWal = wal;
            auto envR = Environment::open(path, opts);
            CHECK(envR.ok());
            if (!envR) return;
            Environment env = envR.moveValue();

            auto vr = backup::verify(env);
            CHECK(vr.ok());
            if (vr.ok()) CHECK(vr.value().ok());

            long long prefixLen = checkPrefixProperty(env, confirmCount + 2);
            CHECK(prefixLen == confirmCount + (expectFinalPresent ? 1 : 0));
            CHECK_OK(env.close());
        };

        constexpr long long kConfirmed = 20;
        runScenario("precommit", false, kConfirmed, /*expectFinalPresent=*/false);
        runScenario("precommit", true, kConfirmed, /*expectFinalPresent=*/false);
        runScenario("prepared", false, kConfirmed, /*expectFinalPresent=*/false);
        runScenario("prepared", true, kConfirmed, /*expectFinalPresent=*/false);
        runScenario("postmetaflip", true, kConfirmed, /*expectFinalPresent=*/true);
    }

    // -------------------------------------------------------------------
    // Tier 4: WAL-based crash recovery of retired free-list pages. Verifies
    // (previously untested) that Wal::recover()'s FreeListUpdate replay
    // (src/wal/wal.cpp) correctly repopulates FreeList::retired_ after a
    // crash, closing the gap README.md used to describe as "process-lifetime
    // only until a backup/compact cycle" -- true only for no-WAL databases.
    // -------------------------------------------------------------------
    {
        constexpr std::uint64_t kSmallMaxDb = 8ull << 20;  // 8 MiB
        constexpr long long kBaseline = 200;
        auto path = anntest::scratchDbPath("crash_tier4_deleteretire.apdb");
        {
            EnvironmentOptions seedOpts;
            seedOpts.maxDatabaseSize = kSmallMaxDb;
            seedOpts.enableWal = true;
            auto envR = Environment::open(path, seedOpts);
            CHECK(envR.ok());
            if (envR.ok()) CHECK_OK(envR.value().close());
        }

        ChildProcess child;
        std::vector<std::string> args{path.string(), "deleteretire", "1", "0", std::to_string(kBaseline)};
        CHECK(spawnProcess(workerExe, args, child));
        int code = waitForExit(child);
        CHECK(code != 0);  // must have crashed, never a clean exit

        EnvironmentOptions opts;
        opts.maxDatabaseSize = kSmallMaxDb;
        opts.enableWal = true;
        auto envR = Environment::open(path, opts);
        CHECK(envR.ok());
        if (envR.ok()) {
            Environment env = envR.moveValue();

            // Every baseline key was deleted (and that delete committed)
            // before the crash.
            for (long long i = 0; i < kBaseline; ++i) {
                CHECK(!env.get(toByteView(keyFor(i))).ok());
            }

            auto vr = backup::verify(env);
            CHECK(vr.ok());
            if (vr.ok()) CHECK(vr.value().ok());

            // Direct proof of FreeListUpdate replay: the delete-commit's own
            // retirements were never promoted before the crash (reclaim()
            // only promotes entries strictly older than the reference
            // txnId, never the committing txn's own), so without replay
            // this would read exactly 0 right after reopening.
            auto statAfterRecovery = env.stat();
            CHECK(statAfterRecovery.ok());
            long long freeAfterRecovery = statAfterRecovery.ok()
                                              ? static_cast<long long>(statAfterRecovery.value().freePages)
                                              : 0;
            CHECK(freeAfterRecovery > 0);

            // One more ordinary commit triggers reclaim(), promoting the
            // replayed retirements to reusable and letting new allocations
            // actually draw from that pool -- assert the free count drops
            // as the reinserted volume consumes it, proving real reuse
            // rather than the engine ignoring the pool and growing past it.
            for (long long i = 0; i < kBaseline; ++i) {
                CHECK_OK(env.put(toByteView(keyFor(i + 1000000)), toByteView(valFor(i + 1000000))));
            }
            auto statAfterReinsert = env.stat();
            CHECK(statAfterReinsert.ok());
            if (statAfterReinsert.ok()) {
                CHECK(static_cast<long long>(statAfterReinsert.value().freePages) < freeAfterRecovery);
            }

            CHECK_OK(env.close());
        }
    }

    TEST_MAIN_END();
}
