//
// AnnPocketDb :: tests/test_wal_replay.cpp
//
// Plain, non-crash WAL happy-path coverage. `test_crash_consistency.cpp`
// already proves WAL recovery is correct *after a hard kill* -- a strictly
// harder scenario -- but that's not a substitute for the simplest possible
// claim: "turn the WAL on, commit some data, close cleanly, reopen, the WAL
// replay path runs and the data is exactly right." A bug that only shows up
// on the ordinary, no-crash-involved replay path (e.g. an off-by-one in
// `RecoveryResult`, a segment/checkpoint bookkeeping error that only
// manifests without a crash mid-stream, or `attachWal` wiring the recovered
// state into `PageManager` incorrectly) would sail straight through the
// crash suite, which never asserts on `RecoveryResult` fields at all and
// always drives its assertions through worker-process kills.
//
// Every case below does a real, clean `Environment::close()` between
// sessions -- there is no crash injection anywhere in this file.
//
#include "test_common.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "annpocketdb/api/environment.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;

namespace {

std::string keyFor(int i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "wk%05d", i);
    return std::string(buf);
}
std::string valFor(int i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "wv%05d", i);
    return std::string(buf);
}

}  // namespace

int main() {
    // ---- 1. Smallest possible case: WAL on, zero commits, clean reopen.
    // Recovery must run cleanly against an empty log (an empty/near-empty
    // WAL directory is exactly the state a program that opens and closes
    // without ever writing anything leaves behind, and it should not be a
    // special-cased failure). ----
    {
        auto path = anntest::scratchDbPath("wal_replay_empty.apdb");
        EnvironmentOptions opts;
        opts.enableWal = true;
        {
            auto envR = Environment::open(path, opts);
            CHECK(envR.ok());
            if (!envR) TEST_MAIN_END();
            Environment env = envR.moveValue();
            CHECK(env.wal() != nullptr);
            CHECK(env.wal()->healthy());
            CHECK_OK(env.close());
        }
        {
            auto envR = Environment::open(path, opts);
            CHECK(envR.ok());
            if (envR.ok()) {
                Environment env = envR.moveValue();
                CHECK(env.wal() != nullptr);
                CHECK(env.wal()->healthy());
                CHECK_OK(env.close());
            }
        }
    }

    // ---- 2. The core claim: commit real data across several transactions,
    // close cleanly, reopen with WAL on, and the data is exactly right --
    // not "roughly right", not "present but requires a repair pass". ----
    {
        auto path = anntest::scratchDbPath("wal_replay_basic.apdb");
        EnvironmentOptions opts;
        opts.enableWal = true;
        constexpr int kCount = 400;

        {
            auto envR = Environment::open(path, opts);
            CHECK(envR.ok());
            if (!envR) TEST_MAIN_END();
            Environment env = envR.moveValue();

            // Several separate commits, not one giant transaction -- this
            // exercises the WAL's per-commit record/flush/checkpoint cycle
            // repeatedly rather than once.
            for (int batch = 0; batch < 4; ++batch) {
                auto wtxnR = env.beginWrite();
                CHECK(wtxnR.ok());
                if (!wtxnR) TEST_MAIN_END();
                txn::WriteTransaction wtxn = wtxnR.moveValue();
                for (int i = batch * 100; i < (batch + 1) * 100; ++i) {
                    CHECK_OK(wtxn.put(toByteView(keyFor(i)), toByteView(valFor(i))));
                }
                CHECK_OK(wtxn.commit());
            }

            auto infoBeforeClose = env.info();
            CHECK(infoBeforeClose.lastTxnId > 0);
            CHECK_OK(env.close());
        }
        {
            auto envR = Environment::open(path, opts);
            CHECK(envR.ok());
            if (!envR) TEST_MAIN_END();
            Environment env = envR.moveValue();

            for (int i = 0; i < kCount; ++i) {
                auto v = env.get(toByteView(keyFor(i)));
                CHECK(v.ok());
                if (v.ok()) CHECK(toStringView(toByteView(v.value())) == valFor(i));
            }
            auto st = env.stat();
            CHECK(st.ok());
            if (st.ok()) CHECK(st.value().entries == static_cast<std::uint64_t>(kCount));

            // A fresh write after reopen must land at a strictly higher txn
            // id than anything replayed -- proves the recovered watermark
            // was actually wired back into the live PageManager, not just
            // used internally during recovery and then discarded.
            auto infoAfterReopen = env.info();
            CHECK_OK(env.put(toByteView("post-reopen"), toByteView("ok")));
            auto infoAfterWrite = env.info();
            CHECK(infoAfterWrite.lastTxnId > infoAfterReopen.lastTxnId);

            CHECK_OK(env.close());
        }
    }

    // ---- 3. Multiple close/reopen cycles in a row, each adding more data
    // on top of what the previous cycle replayed. A bug that only shows up
    // on the *second* recovery (e.g. segment id continuation, or a
    // checkpoint watermark that isn't re-derived correctly from an
    // already-recovered state) would not be caught by a single
    // commit-then-reopen test. ----
    {
        auto path = anntest::scratchDbPath("wal_replay_cycles.apdb");
        EnvironmentOptions opts;
        opts.enableWal = true;
        constexpr int kCycles = 5;
        constexpr int kPerCycle = 50;

        for (int cycle = 0; cycle < kCycles; ++cycle) {
            auto envR = Environment::open(path, opts);
            CHECK(envR.ok());
            if (!envR) continue;
            Environment env = envR.moveValue();

            // Every key written in every earlier cycle must already be
            // visible via ordinary replay-on-open before this cycle writes
            // anything new.
            for (int c = 0; c < cycle; ++c) {
                for (int i = 0; i < kPerCycle; ++i) {
                    int idx = c * kPerCycle + i;
                    auto v = env.get(toByteView(keyFor(idx)));
                    CHECK(v.ok());
                    if (v.ok()) CHECK(toStringView(toByteView(v.value())) == valFor(idx));
                }
            }

            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            if (wtxnR.ok()) {
                txn::WriteTransaction wtxn = wtxnR.moveValue();
                for (int i = 0; i < kPerCycle; ++i) {
                    int idx = cycle * kPerCycle + i;
                    CHECK_OK(wtxn.put(toByteView(keyFor(idx)), toByteView(valFor(idx))));
                }
                CHECK_OK(wtxn.commit());
            }
            CHECK_OK(env.close());
        }

        // Final check after the last cycle's close: every key from every
        // cycle survives one more clean close/reopen.
        {
            auto envR = Environment::open(path, opts);
            CHECK(envR.ok());
            if (envR.ok()) {
                Environment env = envR.moveValue();
                for (int idx = 0; idx < kCycles * kPerCycle; ++idx) {
                    auto v = env.get(toByteView(keyFor(idx)));
                    CHECK(v.ok());
                    if (v.ok()) CHECK(toStringView(toByteView(v.value())) == valFor(idx));
                }
                CHECK_OK(env.close());
            }
        }
    }

    // ---- 4. WAL toggled off after being written with WAL on. Data
    // durability on a *clean* shutdown does not depend on the WAL (it's a
    // recovery/durability accelerant, not the only path to correctness) --
    // the meta-page flip already made the data durable at commit time. A
    // real system will absolutely see this pattern (someone flips
    // enableWal in config without wiping the database), so it needs to
    // keep working, not just "happen to still work today". ----
    {
        auto path = anntest::scratchDbPath("wal_replay_toggle.apdb");
        {
            EnvironmentOptions opts;
            opts.enableWal = true;
            auto envR = Environment::open(path, opts);
            CHECK(envR.ok());
            if (!envR) TEST_MAIN_END();
            Environment env = envR.moveValue();
            for (int i = 0; i < 30; ++i) {
                CHECK_OK(env.put(toByteView(keyFor(i)), toByteView(valFor(i))));
            }
            CHECK_OK(env.close());
        }
        {
            // Reopen with the WAL disabled entirely.
            EnvironmentOptions opts;
            opts.enableWal = false;
            auto envR = Environment::open(path, opts);
            CHECK(envR.ok());
            if (!envR) TEST_MAIN_END();
            Environment env = envR.moveValue();
            CHECK(env.wal() == nullptr);
            for (int i = 0; i < 30; ++i) {
                auto v = env.get(toByteView(keyFor(i)));
                CHECK(v.ok());
                if (v.ok()) CHECK(toStringView(toByteView(v.value())) == valFor(i));
            }
            // Write more without a WAL attached, close, then turn the WAL
            // back on for a third session -- both directions of the toggle.
            CHECK_OK(env.put(toByteView(keyFor(30)), toByteView(valFor(30))));
            CHECK_OK(env.close());
        }
        {
            EnvironmentOptions opts;
            opts.enableWal = true;
            auto envR = Environment::open(path, opts);
            CHECK(envR.ok());
            if (envR.ok()) {
                Environment env = envR.moveValue();
                for (int i = 0; i <= 30; ++i) {
                    auto v = env.get(toByteView(keyFor(i)));
                    CHECK(v.ok());
                    if (v.ok()) CHECK(toStringView(toByteView(v.value())) == valFor(i));
                }
                CHECK_OK(env.close());
            }
        }
    }

    // ---- 5. Segment/checkpoint bookkeeping stays bounded across many
    // ordinary commits. This isn't a hard performance assertion (that would
    // be flaky across CI machines) -- it's a sanity check that checkpointing
    // is actually running on the happy path and not silently accumulating
    // segments forever, which `test_crash_consistency.cpp` would never
    // notice since it never inspects `segmentCount()`. ----
    {
        auto path = anntest::scratchDbPath("wal_replay_checkpoint.apdb");
        EnvironmentOptions opts;
        opts.enableWal = true;
        auto envR = Environment::open(path, opts);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        constexpr int kCommits = 100;
        for (int i = 0; i < kCommits; ++i) {
            CHECK_OK(env.put(toByteView(keyFor(i)), toByteView(valFor(i))));
        }
        CHECK(env.wal() != nullptr);
        if (env.wal() != nullptr) {
            // 100 tiny single-key commits against a 64 MiB default segment
            // size should never need more than a small, bounded handful of
            // segments -- if this ever balloons to "one segment per commit"
            // or similar, checkpointing on the ordinary path has regressed.
            CHECK(env.wal()->segmentCount() <= 4);
        }
        CHECK_OK(env.close());
    }

    // ---- 6. Multi-page overflow chains survive replay. Every reopen
    // replays the WAL up to the last commit, and the PageImage record used
    // to carry no PageHeader::overflowNext -- so replay rewrote every logged
    // overflow page as end-of-chain, and any value spanning more than one
    // page (B-tree large values, blobs) was cut to its first page on disk.
    // Cases 1-5 only ever used ~7-byte values, which is why they never saw
    // it. Reopened twice so replay runs over already-replayed pages too. ----
    {
        auto path = anntest::scratchDbPath("wal_replay_overflow_chain.apdb");
        auto bigValue = [](int i) {
            std::string v(12000 + i * 1000, '\0');  // ~3-4 pages each at 4 KiB
            for (std::size_t j = 0; j < v.size(); ++j) v[j] = static_cast<char>('a' + (j * 7 + i) % 26);
            return v;
        };
        constexpr int kBig = 3;
        ByteBuffer blob(20000);
        for (std::size_t j = 0; j < blob.size(); ++j) blob[j] = static_cast<std::byte>((j * 13) % 251);

        {
            EnvironmentOptions opts;
            opts.enableWal = true;
            auto envR = Environment::open(path, opts);
            CHECK(envR.ok());
            if (!envR) TEST_MAIN_END();
            Environment env = envR.moveValue();
            for (int i = 0; i < kBig; ++i) {
                CHECK_OK(env.put(toByteView(keyFor(i)), toByteView(bigValue(i))));
            }
#if defined(ANNPOCKETDB_WITH_BLOB)
            CHECK(env.putBlob(toByteView(std::string("blob")), toByteView(blob)).ok());
#endif
            CHECK_OK(env.close());
        }
        for (int round = 0; round < 2; ++round) {
            EnvironmentOptions opts;
            opts.enableWal = true;
            auto envR = Environment::open(path, opts);
            CHECK(envR.ok());
            if (!envR) TEST_MAIN_END();
            Environment env = envR.moveValue();
            for (int i = 0; i < kBig; ++i) {
                auto v = env.get(toByteView(keyFor(i)));
                CHECK_OK(v.status());
                if (v.ok()) CHECK(toStringView(toByteView(v.value())) == bigValue(i));
            }
#if defined(ANNPOCKETDB_WITH_BLOB)
            auto b = env.getBlob(toByteView(std::string("blob")));
            CHECK_OK(b.status());
            if (b.ok()) CHECK(b.value() == blob);
#endif
            CHECK_OK(env.close());
        }
    }

    TEST_MAIN_END();
}
