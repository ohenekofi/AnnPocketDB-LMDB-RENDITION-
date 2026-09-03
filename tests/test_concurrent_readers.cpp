//
// AnnPocketDb :: tests/test_concurrent_readers.cpp
//
// Dedicated multi-threaded (in-process) reader test. This is deliberately a
// different job from the two tests that were standing in for it:
//
//   - `test_mvcc.cpp` proves snapshot isolation with exactly ONE reader,
//     single-threaded, against a writer that also runs on the main thread.
//   - `test_mplay_fuzzer.cpp` proves cross-PROCESS contention on the shared
//     lock-file reader table, using real OS processes.
//
// Neither proves the specific thing this file proves: that SEVERAL threads
// in the SAME process, each holding its OWN independently-timed snapshot,
// see fully consistent, mutually-independent views while a writer is
// actively forcing real COW splits concurrently -- i.e. the in-process
// half of the reader-table / free-list-reclaim contract, under real thread
// scheduling, not simulated by a single thread interleaving operations by
// hand.
//
// This is a CORRECTNESS test with fixed, deterministic invariants (not a
// race-hunting soak test -- see test_soak_concurrent.cpp for that). It runs
// a bounded number of operations and asserts exact, checkable properties,
// so it belongs in the default fast ctest run, not behind the `soak` label.
//
#include "test_common.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/btree/cursor.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;

namespace {

constexpr int kBatches = 300;       // writer commits, one batch each
constexpr int kBatchSize = 7;       // new keys per batch
constexpr int kTotalKeys = kBatches * kBatchSize;  // 2100 -- forces real splits

std::string keyFor(int i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "cr%05d", i);
    return std::string(buf);
}
std::string valFor(int i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "cv%05d", i);
    return std::string(buf);
}

// Thread-safe failure reporting: test_common.hpp's CHECK/reportFailure use
// a plain (non-atomic) global counter, which is fine for every other test
// in this suite (all single-threaded) but would itself be a data race if
// called directly from multiple threads here. Collect failures locally per
// thread and fold them into a single CHECK on the main thread after every
// worker has joined.
std::mutex g_logMutex;
std::atomic<int> g_failures{0};

void fail(const std::string& where, const std::string& what) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    std::fprintf(stderr, "[FAIL] test_concurrent_readers (%s): %s\n", where.c_str(), what.c_str());
    ++g_failures;
}

int countEntries(txn::ReadTransaction& txn) {
    btree::Cursor cur = txn.openCursor();
    auto first = cur.first();
    if (!first.ok() || !first.value()) return 0;
    int n = 1;
    while (true) {
        auto next = cur.next();
        if (!next.ok() || !next.value()) break;
        ++n;
    }
    return n;
}

// Writer thread: kBatches sequential commits, each adding kBatchSize brand
// new keys. Keys are only ever added, never mutated or removed, which gives
// every reader thread a simple, checkable invariant: at any point in time,
// the visible key set is *exactly* keys [0, N) for some N that is a
// multiple of kBatchSize -- never a partial batch, never a hole.
void writerThread(Environment* env) {
    for (int batch = 0; batch < kBatches; ++batch) {
        auto wtxnR = env->beginWrite();
        if (!wtxnR.ok()) {
            fail("writerThread", "beginWrite failed: " + wtxnR.status().toString());
            return;
        }
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        for (int i = 0; i < kBatchSize; ++i) {
            int idx = batch * kBatchSize + i;
            Status s = wtxn.put(toByteView(keyFor(idx)), toByteView(valFor(idx)));
            if (!s.ok()) fail("writerThread", "put failed: " + s.toString());
        }
        Status cs = wtxn.commit();
        if (!cs.ok()) fail("writerThread", "commit failed: " + cs.toString());
    }
}

// "Hammer" reader thread: repeatedly opens a brand-new ReadTransaction,
// checks the atomicity/no-holes invariant against whatever snapshot it
// happens to land on, then closes it and does it again. Each thread also
// checks that ITS OWN sequence of observed counts is non-decreasing --
// snapshots this same thread opens later can only ever see equal or more
// data than one it opened earlier, never less (env only ever grows here).
void hammerReaderThread(Environment* env, int iterations) {
    int lastSeenCount = -1;
    for (int iter = 0; iter < iterations; ++iter) {
        auto rtxnR = env->beginRead();
        if (!rtxnR.ok()) {
            fail("hammerReaderThread", "beginRead failed: " + rtxnR.status().toString());
            continue;
        }
        txn::ReadTransaction rtxn = rtxnR.moveValue();
        int n = countEntries(rtxn);

        if (n % kBatchSize != 0) {
            fail("hammerReaderThread",
                 "saw a partial batch: " + std::to_string(n) + " entries (not a multiple of " +
                     std::to_string(kBatchSize) + ") -- a commit is visible only partway through");
            continue;
        }
        if (lastSeenCount >= 0 && n < lastSeenCount) {
            fail("hammerReaderThread", "entry count went backwards: " + std::to_string(lastSeenCount) +
                                            " -> " + std::to_string(n));
        }
        lastSeenCount = n;

        // No-holes check: every key in [0, n) must be present with the
        // exact expected value; nothing beyond n may be present yet.
        for (int i = 0; i < n; ++i) {
            auto v = rtxn.get(toByteView(keyFor(i)));
            if (!v.ok()) {
                fail("hammerReaderThread", "hole at key " + std::to_string(i) + " under count " + std::to_string(n));
                continue;
            }
            if (toStringView(toByteView(v.value())) != valFor(i)) {
                fail("hammerReaderThread", "wrong value at key " + std::to_string(i));
            }
        }
        if (n < kTotalKeys) {
            auto beyond = rtxn.get(toByteView(keyFor(n)));
            if (beyond.ok()) {
                fail("hammerReaderThread", "key " + std::to_string(n) + " visible before its batch committed");
            }
        }
    }
}

// "Pinned" reader thread: opens exactly ONE snapshot as early as possible
// (before the writer necessarily has done anything) and holds it for the
// entire test, re-verifying at the very end that it is still exactly what
// it was when opened -- the multi-threaded generalization of test_mvcc's
// single-threaded pinned-snapshot check, run concurrently with a real
// writer and other concurrent readers instead of being the only thing
// touching the environment.
void pinnedReaderThread(Environment* env, int* outBaselineCount) {
    auto rtxnR = env->beginRead();
    if (!rtxnR.ok()) {
        fail("pinnedReaderThread", "beginRead failed: " + rtxnR.status().toString());
        *outBaselineCount = -1;
        return;
    }
    txn::ReadTransaction rtxn = rtxnR.moveValue();
    int baseline = countEntries(rtxn);
    *outBaselineCount = baseline;

    // Hold the transaction open (do not let it go out of scope) while the
    // writer thread runs the entire batch sequence, spot-checking every
    // little while that nothing has moved underneath it.
    for (int check = 0; check < 20; ++check) {
        int n = countEntries(rtxn);
        if (n != baseline) {
            fail("pinnedReaderThread",
                 "snapshot count changed from " + std::to_string(baseline) + " to " + std::to_string(n) +
                     " while pinned");
        }
        for (int i = 0; i < baseline; ++i) {
            auto v = rtxn.get(toByteView(keyFor(i)));
            if (!v.ok() || toStringView(toByteView(v.value())) != valFor(i)) {
                fail("pinnedReaderThread", "value for key " + std::to_string(i) + " changed under a held snapshot");
                break;
            }
        }
        std::this_thread::yield();
    }
}

}  // namespace

int main() {
    auto path = anntest::scratchDbPath("concurrent_readers.apdb");
    auto envR = Environment::open(path);
    CHECK(envR.ok());
    if (!envR) TEST_MAIN_END();
    Environment env = envR.moveValue();

    int pinnedBaselineA = -1;
    int pinnedBaselineB = -1;

    std::vector<std::thread> threads;
    // Two pinned readers, staggered only by natural thread-start jitter --
    // each independently proves the isolation guarantee holds no matter
    // when exactly its snapshot happened to land relative to the writer.
    threads.emplace_back(pinnedReaderThread, &env, &pinnedBaselineA);
    threads.emplace_back(pinnedReaderThread, &env, &pinnedBaselineB);
    // Two hammer readers, opening/closing many short-lived snapshots
    // throughout the writer's entire run.
    threads.emplace_back(hammerReaderThread, &env, 400);
    threads.emplace_back(hammerReaderThread, &env, 400);
    // The writer itself.
    threads.emplace_back(writerThread, &env);

    for (auto& t : threads) t.join();

    CHECK(g_failures.load() == 0);

    // Both pinned readers must have opened successfully (not necessarily
    // the same baseline as each other -- they may have started a few
    // batches apart -- but each must be a valid multiple of kBatchSize).
    CHECK(pinnedBaselineA >= 0 && pinnedBaselineA % kBatchSize == 0);
    CHECK(pinnedBaselineB >= 0 && pinnedBaselineB % kBatchSize == 0);

    // After every thread has joined, a fresh snapshot must see everything.
    {
        auto rtxnR = env.beginRead();
        CHECK(rtxnR.ok());
        if (rtxnR.ok()) {
            txn::ReadTransaction rtxn = rtxnR.moveValue();
            CHECK(countEntries(rtxn) == kTotalKeys);
            for (int i = 0; i < kTotalKeys; i += 97) {  // sparse spot-check, full scan is above
                auto v = rtxn.get(toByteView(keyFor(i)));
                CHECK(v.ok());
                if (v.ok()) CHECK(toStringView(toByteView(v.value())) == valFor(i));
            }
        }
    }

    CHECK_OK(env.close());
    TEST_MAIN_END();
}
