// mplay_worker: scripted single-process reader/writer for the multi-process
// fuzzer driver (test_mplay_fuzzer.cpp). A real, separate OS process --
// spawned via process_spawn.hpp, never a thread -- so together with its
// siblings it exercises the .lock file's shared-memory reader table (see
// platform/interprocess_lock.hpp) under genuine cross-process contention.
//
// argv: mplay_worker <dbPath> <role> <iterations> <seed>
//   role: "writer" (put a random key from a fixed keyspace each iteration)
//      or "reader" (read every key each iteration, verifying well-formedness)
//
// Exit code 0 = clean; non-zero = an error or a detected inconsistency
// (torn/garbage read, unexpected Status). stderr carries the reason.
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

#include "annpocketdb/api/environment.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;

namespace {

constexpr int kKeyCount = 20;

std::string keyName(int i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "mplay%03d", i);
    return std::string(buf);
}

// Every value a writer ever stores has this exact shape: "v" + 6 digits.
// A reader seeing anything else read back a torn/garbage value -- the whole
// point of this fuzzer is to prove that never happens under real
// cross-process contention.
bool isWellFormedValue(const std::string& v) {
    if (v.size() != 7 || v[0] != 'v') return false;
    for (std::size_t i = 1; i < v.size(); ++i) {
        if (v[i] < '0' || v[i] > '9') return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: mplay_worker <dbPath> <role> <iterations> <seed>\n");
        return 2;
    }
    std::string dbPath = argv[1];
    std::string role = argv[2];
    int iterations = std::atoi(argv[3]);
    unsigned seed = static_cast<unsigned>(std::atoi(argv[4]));

    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> keyDist(0, kKeyCount - 1);

    EnvironmentOptions opts;
    opts.multiProcess = true;
    // The default 1 TiB virtual reservation ceiling is fine for a single
    // process, but Windows charges system commit for a PAGE_READWRITE file
    // mapping regardless of file-backing -- 12 worker processes each
    // reserving 1 TiB concurrently can exhaust the paging-file-backed commit
    // limit under load (observed: MapViewOfFile failing with
    // GetLastError()==1455/ERROR_COMMIT_LIMIT). This test's entire dataset
    // (kKeyCount small keys/values) never remotely approaches even 1 MiB, so
    // a much smaller ceiling costs nothing and removes the failure mode.
    opts.maxDatabaseSize = 64ull << 20;  // 64 MiB
    auto envR = Environment::open(dbPath, opts);
    if (!envR) {
        std::fprintf(stderr, "mplay_worker: open failed: %s\n", envR.status().toString().c_str());
        return 1;
    }
    Environment env = envR.moveValue();

    if (role == "writer") {
        int seq = seed * 100000;  // keep each writer process's sequence numbers distinguishable
        for (int i = 0; i < iterations; ++i) {
            int k = keyDist(rng);
            ++seq;
            char valBuf[8];
            std::snprintf(valBuf, sizeof(valBuf), "v%06d", seq % 1000000);
            Status s = env.put(toByteView(keyName(k)), toByteView(std::string(valBuf)));
            if (!s.ok()) {
                std::fprintf(stderr, "mplay_worker: put failed: %s\n", s.toString().c_str());
                return 1;
            }
        }
    } else if (role == "reader") {
        for (int i = 0; i < iterations; ++i) {
            for (int k = 0; k < kKeyCount; ++k) {
                auto v = env.get(toByteView(keyName(k)));
                if (!v.ok()) {
                    if (v.status().code() == StatusCode::NotFound) continue;  // not written yet -- fine
                    std::fprintf(stderr, "mplay_worker: get failed: %s\n", v.status().toString().c_str());
                    return 1;
                }
                std::string s(reinterpret_cast<const char*>(v.value().data()), v.value().size());
                if (!isWellFormedValue(s)) {
                    std::fprintf(stderr, "mplay_worker: torn/garbage read for key %d: '%s'\n", k, s.c_str());
                    return 1;
                }
            }
        }
    } else {
        std::fprintf(stderr, "mplay_worker: unknown role '%s'\n", role.c_str());
        return 2;
    }

    Status cs = env.close();
    if (!cs.ok()) {
        std::fprintf(stderr, "mplay_worker: close failed: %s\n", cs.toString().c_str());
        return 1;
    }
    return 0;
}
