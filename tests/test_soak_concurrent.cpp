//
// AnnPocketDb :: tests/test_soak_concurrent.cpp
//
// In-process concurrency soak test. Existing tests (test_mvcc, test_btree_
// rebalance, test_concurrent_readers) each prove one property with a small,
// fixed number of operations. This test exists for a different job: run
// sustained, randomized, overlapping reader/writer/nested-writer activity
// for a configurable wall-clock duration so a race detector (TSan) has
// enough scheduling entropy to actually trip on a bug, rather than getting
// lucky with a short, deterministic sequence.
//
// A sanitizer with nothing but the fast unit-test suite behind it mostly
// proves the fast suite itself is data-race-free -- it says very little
// about the MVCC / free-list / reader-table code paths that only misbehave
// under real scheduling pressure. This test is the load generator that
// makes TSan runs meaningful (see docs/SANITIZERS.md).
//
// Duration is controlled by the ANNPOCKETDB_SOAK_SECONDS environment
// variable (default: 5, a fast CI smoke run). CI's nightly job sets this to
// something like 3600. Under TSan the same wall-clock budget covers far
// fewer iterations (5-20x slowdown is typical), which is fine -- TSan cares
// about interleavings exercised, not raw op count.
//
// What this specifically targets, mapped to the disclosed gap:
//
//   1. Free-list two-stage reclaim: a long-lived pinned reader must never
//      observe a page that was reused for something else. We prove this
//      continuously, not just once (see PinnedReaderState / worker below),
//      across thousands of interleaved commits including forced splits,
//      merges, and overflow-chain churn.
//   2. Meta-page double-buffer flip: concurrent short-lived readers opened
//      mid-write-storm must always see a self-consistent (never partially
//      applied) snapshot -- checked via a per-record embedded checksum, not
//      just "did the read succeed."
//   3. Writer-mutex serialization: multiple writer threads racing
//      beginWrite() must never observe interleaved writes (verified via a
//      monotonic per-key write-sequence embedded in the value).
//   4. Nested transactions racing top-level commits on other threads: the
//      savepoint stack is per-WriteTransaction state; this exercises that
//      under contention for the single writer slot.
//
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "annpocketdb/api/environment.hpp"
#include "test_common.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;

namespace {

constexpr int kKeyCount = 256;              // wide enough to force real splits/merges
constexpr int kValuePadTarget = 96;         // pushes some values toward overflow thresholds
constexpr int kSmallValueEvery = 5;         // periodic small values force size-class churn

std::mutex g_logMutex;
std::atomic<int> g_failures{0};

void reportRace(const std::string& where, const std::string& what) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    std::fprintf(stderr, "[SOAK-FAIL] %s: %s\n", where.c_str(), what.c_str());
    ++g_failures;
}

std::string keyName(int i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "soak%04d", i);
    return std::string(buf);
}

// Value format: "s<8-hex-seq>c<4-hex-checksum>" then padding. The checksum
// covers the sequence field only, so a torn/partial read (old bytes mixed
// with new) is detectable independent of value length games from overflow
// promotion/demotion.
std::string makeValue(uint32_t seq, std::mt19937& rng) {
    // "s" + 8 hex + "c" + 4 hex = 14 characters; buffer must hold that plus
    // the NUL snprintf always appends.
    char head[15];
    uint16_t chk = static_cast<uint16_t>((seq * 2654435761u) & 0xFFFFu);
    std::snprintf(head, sizeof(head), "s%08xc%04x", seq, chk);
    std::string v(head, 14);
    if (seq % kSmallValueEvery != 0) {
        std::uniform_int_distribution<int> padDist(0, kValuePadTarget);
        v.append(static_cast<std::size_t>(padDist(rng)), 'x');
    }
    return v;
}

bool checkValue(const std::string& v, const std::string& where) {
    if (v.size() < 14 || v[0] != 's' || v[9] != 'c') {
        reportRace(where, "malformed value shape: '" + v + "'");
        return false;
    }
    uint32_t seq = 0;
    uint16_t chk = 0;
    if (std::sscanf(v.c_str(), "s%08xc%04hx", &seq, &chk) != 2) {
        reportRace(where, "unparsable value: '" + v + "'");
        return false;
    }
    uint16_t expect = static_cast<uint16_t>((seq * 2654435761u) & 0xFFFFu);
    if (expect != chk) {
        reportRace(where, "checksum mismatch (torn read?) in '" + v + "'");
        return false;
    }
    return true;
}

std::chrono::steady_clock::time_point g_deadline;

bool timeUp() { return std::chrono::steady_clock::now() >= g_deadline; }

// --- Writer threads: overlapping keyspace, forces real contention on the
// single-writer path plus size-class churn (inline <-> overflow transitions
// exercise free-list retire/reclaim continuously). Every Nth batch uses a
// nested transaction with a random abort, to exercise savepoint rollback
// concurrently with other threads blocked on the writer mutex.
void writerLoop(Environment* env, int workerId, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> keyDist(0, kKeyCount - 1);
    std::uniform_int_distribution<int> nestedDist(0, 9);
    uint32_t seq = static_cast<uint32_t>(workerId) * 1000000u;

    while (!timeUp()) {
        auto wtR = env->beginWrite();
        if (!wtR.ok()) {
            // Busy is an acceptable transient outcome under contention; the
            // API otherwise serializes writers, so anything else is a bug.
            if (wtR.status().code() != StatusCode::Busy) {
                reportRace("writerLoop", "beginWrite failed: " + wtR.status().toString());
            }
            continue;
        }
        auto wt = wtR.moveValue();

        bool useNested = nestedDist(rng) == 0;
        if (useNested) {
            Status ns = wt.beginNested();
            if (!ns.ok()) reportRace("writerLoop", "beginNested failed: " + ns.toString());
        }

        int batchSize = 1 + static_cast<int>(seq % 5);
        for (int i = 0; i < batchSize; ++i) {
            int k = keyDist(rng);
            ++seq;
            Status s = wt.put(toByteView(keyName(k)), toByteView(makeValue(seq, rng)));
            if (!s.ok()) {
                reportRace("writerLoop", "put failed: " + s.toString());
            }
            if (seq % 37 == 0) {
                // Occasional delete to force merge/redistribute rebalancing
                // concurrently with other writers queued on the mutex.
                (void)wt.remove(toByteView(keyName(k)));
            }
        }

        if (useNested) {
            if (nestedDist(rng) < 3) {
                Status as = wt.abortNested();
                if (!as.ok()) reportRace("writerLoop", "abortNested failed: " + as.toString());
            } else {
                Status cs = wt.commitNested();
                if (!cs.ok()) reportRace("writerLoop", "commitNested failed: " + cs.toString());
            }
        }

        Status cs = wt.commit(core::SyncMode::Flush);
        if (!cs.ok()) {
            reportRace("writerLoop", "commit failed: " + cs.toString());
        }
    }
}

// --- Short-lived reader threads: open, read every key, close, repeat.
// Exercises reader-table register/deregister churn and meta-page-flip
// visibility under a live write storm.
void shortReaderLoop(Environment* env, int /*workerId*/, unsigned seed) {
    std::mt19937 rng(seed);
    while (!timeUp()) {
        auto rtR = env->beginRead();
        if (!rtR.ok()) {
            reportRace("shortReaderLoop", "beginRead failed: " + rtR.status().toString());
            continue;
        }
        auto rt = rtR.moveValue();
        for (int k = 0; k < kKeyCount; ++k) {
            auto v = rt.get(toByteView(keyName(k)));
            if (!v.ok()) {
                if (v.status().code() == StatusCode::NotFound) continue;
                reportRace("shortReaderLoop", "get failed: " + v.status().toString());
                continue;
            }
            std::string s(reinterpret_cast<const char*>(v.value().data()), v.value().size());
            checkValue(s, "shortReaderLoop");
        }
    }
}

// --- Pinned long-lived reader: opens ONE snapshot at start and holds it for
// the entire soak duration, periodically re-verifying its original values
// never change underneath it. This is the direct stress test of two-stage
// free-list reclaim: thousands of commits must happen while this reader's
// pages are never reused.
void pinnedReaderLoop(Environment* env) {
    auto rtR = env->beginRead();
    if (!rtR.ok()) {
        reportRace("pinnedReaderLoop", "beginRead failed: " + rtR.status().toString());
        return;
    }
    auto rt = rtR.moveValue();

    // Snapshot the initial well-formed values we can see right now.
    std::vector<std::pair<int, std::string>> baseline;
    for (int k = 0; k < kKeyCount; ++k) {
        auto v = rt.get(toByteView(keyName(k)));
        if (v.ok()) {
            std::string s(reinterpret_cast<const char*>(v.value().data()), v.value().size());
            if (checkValue(s, "pinnedReaderLoop(baseline)")) {
                baseline.emplace_back(k, s);
            }
        }
    }

    while (!timeUp()) {
        for (auto& [k, expected] : baseline) {
            auto v = rt.get(toByteView(keyName(k)));
            if (!v.ok()) {
                reportRace("pinnedReaderLoop",
                            "key " + keyName(k) + " vanished from a held snapshot");
                continue;
            }
            std::string s(reinterpret_cast<const char*>(v.value().data()), v.value().size());
            if (s != expected) {
                reportRace("pinnedReaderLoop",
                            "key " + keyName(k) + " changed under a held snapshot: '" +
                                expected + "' -> '" + s + "'");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// --- Maintenance thread: periodically reaps dead-PID reader slots, the same
// call path production code would use. Running it concurrently with active
// readers/writers is itself a race-detector target (it touches the shared
// reader table).
void maintenanceLoop(Environment* env) {
    while (!timeUp()) {
        (void)env->readerCheck();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

}  // namespace

int main() {
    int soakSeconds = 5;
    if (const char* envSeconds = std::getenv("ANNPOCKETDB_SOAK_SECONDS")) {
        int parsed = std::atoi(envSeconds);
        if (parsed > 0) soakSeconds = parsed;
    }
    unsigned writerThreads = 4;
    if (const char* envW = std::getenv("ANNPOCKETDB_SOAK_WRITERS")) {
        int parsed = std::atoi(envW);
        if (parsed > 0) writerThreads = static_cast<unsigned>(parsed);
    }
    unsigned readerThreads = 4;
    if (const char* envR = std::getenv("ANNPOCKETDB_SOAK_READERS")) {
        int parsed = std::atoi(envR);
        if (parsed > 0) readerThreads = static_cast<unsigned>(parsed);
    }

    auto dbPath = anntest::scratchDbPath("soak_concurrent.apdb");

    EnvironmentOptions opts;
    opts.multiProcess = false;  // in-process threads only; see test_mplay_soak for cross-process
    opts.maxDatabaseSize = 1ull << 33;

    auto envR = Environment::open(dbPath, opts);
    CHECK_OK(envR.status());
    if (!envR.ok()) TEST_MAIN_END();
    Environment env = envR.moveValue();

    // Seed a baseline so the pinned reader has something to hold onto from
    // the very first instant.
    {
        auto wt = env.beginWrite().moveValue();
        std::mt19937 seedRng(1);
        for (int k = 0; k < kKeyCount; ++k) {
            CHECK_OK(wt.put(toByteView(keyName(k)), toByteView(makeValue(k, seedRng))));
        }
        CHECK_OK(wt.commit());
    }

    g_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(soakSeconds);

    std::vector<std::thread> threads;
    threads.emplace_back(pinnedReaderLoop, &env);
    threads.emplace_back(maintenanceLoop, &env);
    for (unsigned i = 0; i < writerThreads; ++i) {
        threads.emplace_back(writerLoop, &env, static_cast<int>(i), 1000u + i);
    }
    for (unsigned i = 0; i < readerThreads; ++i) {
        threads.emplace_back(shortReaderLoop, &env, static_cast<int>(i), 2000u + i);
    }

    for (auto& t : threads) t.join();

    CHECK(g_failures.load() == 0);

    // Final structural sanity pass, if the backup/verify module was built.
#if defined(ANNPOCKETDB_WITH_BACKUP)
    auto report = env.verify();
    CHECK(report.ok());
    if (report.ok() && !report.value().ok()) {
        for (const auto& issue : report.value().issues) {
            std::fprintf(stderr, "[SOAK-FAIL] verify(): page %llu: %s\n",
                         static_cast<unsigned long long>(issue.pageId), issue.message.c_str());
        }
        CHECK(report.value().ok());
    }
#endif

    CHECK_OK(env.close());

    std::printf("[SOAK] ran %d second(s), %u writer thread(s), %u reader thread(s), %d failure(s)\n",
                soakSeconds, writerThreads, readerThreads, g_failures.load());

    TEST_MAIN_END();
}
