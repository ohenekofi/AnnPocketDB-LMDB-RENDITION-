// crash_worker: scripted single-process writer for the crash-consistency
// test suite (test_crash_consistency.cpp). A real, separate OS process
// (spawned via process_spawn.hpp) so it can be genuinely hard-killed
// (TerminateProcess/SIGKILL) or self-terminate (_exit/TerminateProcess on
// itself) mid-operation -- neither is meaningfully simulable from inside the
// same process as the test driver.
//
// argv: crash_worker <dbPath> <scenario> <wal:0|1> <startIndex> <count>
//   Keys are "ck%06d" -> "val%06d", strictly sequential, one commit each.
//   Every scenario first performs `count` ordinary, fully-committed
//   put()s for indices [startIndex, startIndex+count) -- this is the
//   confirmed prefix the test driver already knows about without needing
//   to ask the worker anything further.
//
//   scenario:
//     loop        - keep committing indices starting at startIndex+count
//                   until externally killed (or an unreasonably large cap is
//                   hit). After every successful commit, overwrites
//                   <dbPath>.progress with that index (a conservative lower
//                   bound on confirmed state for the driver to check after
//                   an external kill; never fsynced, never load-bearing for
//                   the engine's own durability -- purely test bookkeeping).
//     precommit   - begins one more write (index startIndex+count), calls
//                   put() on it, then hard-crashes *without* calling
//                   commit(). Expect: that key never appears after recovery.
//     prepared    - same, but calls wtxn.prepare() before hard-crashing
//                   without commit()/abort(). Expect: same as precommit --
//                   a lone Prepared meta slot must never be mistaken for
//                   committed.
//     postmetaflip - requires wal=1. Installs a PageManager test hook (see
//                   core::PageManager::setTestPostMetaFlipHook) that
//                   hard-crashes from *inside* commit(), immediately after
//                   the meta flip is published but before the WAL
//                   flush/checkpoint step. Then commits index
//                   startIndex+count normally. Expect: that key *does*
//                   appear after recovery, reconstructed via WAL replay,
//                   even though checkpoint/data-fsync never ran.
//     deleteretire - requires wal=1. After the `count` confirmed inserts,
//                   deletes every one of those keys in one more commit
//                   (COW-retires all their pages, logged as a single
//                   FreeListUpdate WAL record), then hard-crashes
//                   immediately -- no further commit ever runs, so
//                   FreeList::reclaim never gets a chance to promote this
//                   commit's own retirements (it only promotes entries with
//                   retiringTxnId < the reference txnId, never ==). Proves
//                   core::PageManager/wal::Wal's existing (previously
//                   untested) FreeListUpdate-replay-on-recovery mechanism:
//                   the driver expects env.stat().freePages > 0 immediately
//                   after reopening, before it does anything else.
//
// Exit code 0 = the worker finished on its own (only "loop" with a small
// count, or if it somehow wasn't killed); non-zero/never-returns = crashed
// as intended. stderr carries diagnostics for anything unexpected.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "annpocketdb/api/environment.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace annpocketdb;
using namespace annpocketdb::api;

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

// A real crash: no destructors, no flush, no atexit handlers. Never returns.
[[noreturn]] void hardCrash(int code) {
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
#else
    _exit(code);
#endif
    for (;;) {}  // unreachable; satisfies [[noreturn]] on all toolchains
}

void writeProgress(const std::string& dbPath, long long lastCommitted) {
    std::ofstream f(dbPath + ".progress", std::ios::trunc);
    f << lastCommitted;
}

// Set as PageManager's test hook for the "postmetaflip" scenario. Must be a
// plain function pointer (no captured state) -- see setTestPostMetaFlipHook.
[[noreturn]] void postMetaFlipCrashHook() { hardCrash(1); }

}  // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: crash_worker <dbPath> <scenario> <wal:0|1> <startIndex> <count>\n");
        return 2;
    }
    std::string dbPath = argv[1];
    std::string scenario = argv[2];
    bool useWal = std::atoi(argv[3]) != 0;
    long long startIndex = std::atoll(argv[4]);
    long long count = std::atoll(argv[5]);

    EnvironmentOptions opts;
    opts.enableWal = useWal;
    // The default 1 TiB virtual reservation ceiling makes Windows charge
    // system commit for the mapping regardless of file-backing (see the
    // mplay_fuzzer ERROR_COMMIT_LIMIT fix elsewhere in this suite) -- with
    // dozens of these worker processes spawned across a single
    // test_crash_consistency run, that adds up fast. Must match (or exceed)
    // whatever the driver used to seed/reopen the same file with.
    opts.maxDatabaseSize = 8ull << 20;  // 8 MiB
    auto envR = Environment::open(dbPath, opts);
    if (!envR) {
        std::fprintf(stderr, "crash_worker: open failed: %s\n", envR.status().toString().c_str());
        return 1;
    }
    Environment env = envR.moveValue();

    // Every scenario starts with `count` ordinary, fully-confirmed commits.
    for (long long i = startIndex; i < startIndex + count; ++i) {
        Status s = env.put(toByteView(keyFor(i)), toByteView(valFor(i)));
        if (!s.ok()) {
            std::fprintf(stderr, "crash_worker: put failed at %lld: %s\n", i, s.toString().c_str());
            return 1;
        }
    }
    long long finalIndex = startIndex + count;

    if (scenario == "loop") {
        // Keep going until externally killed. The cap just bounds worst-case
        // runtime if the driver's kill somehow never lands.
        constexpr long long kCap = 200000;
        for (long long i = finalIndex; i < finalIndex + kCap; ++i) {
            Status s = env.put(toByteView(keyFor(i)), toByteView(valFor(i)));
            if (!s.ok()) {
                std::fprintf(stderr, "crash_worker: put failed at %lld: %s\n", i, s.toString().c_str());
                return 1;
            }
            writeProgress(dbPath, i);
        }
        Status cs = env.close();
        return cs.ok() ? 0 : 1;
    }

    if (scenario == "precommit" || scenario == "prepared") {
        auto wtxnR = env.beginWrite();
        if (!wtxnR) {
            std::fprintf(stderr, "crash_worker: beginWrite failed: %s\n", wtxnR.status().toString().c_str());
            return 1;
        }
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        Status ps = wtxn.put(toByteView(keyFor(finalIndex)), toByteView(valFor(finalIndex)));
        if (!ps.ok()) {
            std::fprintf(stderr, "crash_worker: final put failed: %s\n", ps.toString().c_str());
            return 1;
        }
        if (scenario == "prepared") {
            Status prep = wtxn.prepare();
            if (!prep.ok()) {
                std::fprintf(stderr, "crash_worker: prepare failed: %s\n", prep.toString().c_str());
                return 1;
            }
        }
        // Deliberately never call commit()/abort(): simulate a crash right
        // here, mid-transaction (precommit) or durably-prepared-but-
        // undecided (prepared).
        hardCrash(1);
    }

    if (scenario == "postmetaflip") {
        if (!useWal) {
            std::fprintf(stderr, "crash_worker: postmetaflip requires wal=1\n");
            return 2;
        }
        auto wtxnR = env.beginWrite();
        if (!wtxnR) {
            std::fprintf(stderr, "crash_worker: beginWrite failed: %s\n", wtxnR.status().toString().c_str());
            return 1;
        }
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        Status ps = wtxn.put(toByteView(keyFor(finalIndex)), toByteView(valFor(finalIndex)));
        if (!ps.ok()) {
            std::fprintf(stderr, "crash_worker: final put failed: %s\n", ps.toString().c_str());
            return 1;
        }
        wtxn.pageManager().setTestPostMetaFlipHook(&postMetaFlipCrashHook);
        // commit() publishes the meta flip, then the hook fires from inside
        // it (before flush/checkpoint) and hard-crashes -- this call never
        // returns.
        (void)wtxn.commit();
        // Unreachable: the hook always crashes before commit() can return.
        std::fprintf(stderr, "crash_worker: postmetaflip hook did not fire\n");
        return 1;
    }

    if (scenario == "deleteretire") {
        if (!useWal) {
            std::fprintf(stderr, "crash_worker: deleteretire requires wal=1\n");
            return 2;
        }
        auto wtxnR = env.beginWrite();
        if (!wtxnR) {
            std::fprintf(stderr, "crash_worker: beginWrite failed: %s\n", wtxnR.status().toString().c_str());
            return 1;
        }
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        for (long long i = startIndex; i < finalIndex; ++i) {
            Status rs = wtxn.remove(toByteView(keyFor(i)));
            if (!rs.ok()) {
                std::fprintf(stderr, "crash_worker: remove failed at %lld: %s\n", i, rs.toString().c_str());
                return 1;
            }
        }
        Status cs = wtxn.commit();
        if (!cs.ok()) {
            std::fprintf(stderr, "crash_worker: delete commit failed: %s\n", cs.toString().c_str());
            return 1;
        }
        // Crash immediately -- no further commit ever runs, so this
        // transaction's own retirements are guaranteed to still be sitting
        // in FreeList::retired_, unpromoted, when the process dies.
        hardCrash(1);
    }

    std::fprintf(stderr, "crash_worker: unknown scenario '%s'\n", scenario.c_str());
    return 2;
}
