// Value/overflow-level LZ4 compression (see compression/lz4_codec.hpp,
// BTree::maybeCompress). Verifies:
//   - exact round-trip across a size matrix spanning below/at/above
//     compressionMinSize and below/at/above the overflow threshold
//   - an incompressible (random-bytes) value still round-trips correctly
//     even though it won't shrink (falls back to uncompressed storage)
//   - compression produces *real* disk savings: a highly-repetitive large
//     value uses materially fewer overflow pages with compression on than
//     off, proving genuine compression happened, not just format bookkeeping
//   - DUPSORT and DUPFIXED values are completely unaffected (never
//     compressed) even with compression enabled env-wide at an aggressive
//     (tiny) minimum size
#include "test_common.hpp"

#include <algorithm>
#include <cstring>
#include <random>
#include <string>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/btree/db_flags.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;
using namespace annpocketdb::btree;

namespace {
ByteBuffer repetitive(std::size_t n, unsigned char c) {
    ByteBuffer b(n);
    for (auto& x : b) x = static_cast<std::byte>(c);
    return b;
}
ByteBuffer randomBytes(std::size_t n, std::uint32_t seed) {
    ByteBuffer b(n);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto& x : b) x = static_cast<std::byte>(static_cast<unsigned char>(dist(rng)));
    return b;
}
bool bytesEqual(ByteView a, ByteView b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}
}  // namespace

int main() {
    // ---- Round-trip across the size/threshold matrix ----
    {
        auto path = anntest::scratchDbPath("compression_roundtrip.apdb");
        EnvironmentOptions opts;
        opts.enableCompression = true;
        opts.compressionMinSize = 256;
        auto envR = Environment::open(path, opts);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        // 64/255: below compressionMinSize, never compressed.
        // 300/1000: above minSize, still inline (well below the overflow
        // threshold, which is payloadCapacity/4 -- ~1KB for a 4KB page).
        // 20000: forces the overflow chain regardless of compression.
        const std::size_t sizes[] = {64, 255, 300, 1000, 20000};
        int idx = 0;
        for (std::size_t sz : sizes) {
            std::string key = "key" + std::to_string(idx);
            ByteBuffer val = repetitive(sz, static_cast<unsigned char>('A' + (idx % 26)));
            CHECK_OK(env.put(toByteView(key), toByteView(val)));
            auto got = env.get(toByteView(key));
            CHECK(got.ok());
            if (got.ok()) CHECK(bytesEqual(toByteView(got.value()), toByteView(val)));
            ++idx;
        }
        CHECK_OK(env.close());
    }

    // ---- Incompressible (random) value still round-trips ----
    {
        auto path = anntest::scratchDbPath("compression_incompressible.apdb");
        EnvironmentOptions opts;
        opts.enableCompression = true;
        opts.compressionMinSize = 32;
        auto envR = Environment::open(path, opts);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        ByteBuffer val = randomBytes(2000, 0xC0FFEEu);
        CHECK_OK(env.put(toByteView("r"), toByteView(val)));
        auto got = env.get(toByteView("r"));
        CHECK(got.ok());
        if (got.ok()) CHECK(bytesEqual(toByteView(got.value()), toByteView(val)));
        CHECK_OK(env.close());
    }

    // ---- Real disk savings: fewer overflow pages with compression on ----
    {
        ByteBuffer bigVal = repetitive(200000, 'Z');  // ~49 overflow pages uncompressed at 4KB pages

        auto offPath = anntest::scratchDbPath("compression_off.apdb");
        std::uint64_t overflowPagesOff = 0;
        {
            auto envR = Environment::open(offPath);
            CHECK(envR.ok());
            if (!envR) TEST_MAIN_END();
            Environment env = envR.moveValue();
            CHECK_OK(env.put(toByteView("big"), toByteView(bigVal)));
            auto st = env.stat();
            CHECK(st.ok());
            if (st.ok()) overflowPagesOff = st.value().overflowPages;
            CHECK_OK(env.close());
        }

        auto onPath = anntest::scratchDbPath("compression_on.apdb");
        std::uint64_t overflowPagesOn = 0;
        {
            EnvironmentOptions opts;
            opts.enableCompression = true;
            opts.compressionMinSize = 256;
            auto envR = Environment::open(onPath, opts);
            CHECK(envR.ok());
            if (!envR) TEST_MAIN_END();
            Environment env = envR.moveValue();
            CHECK_OK(env.put(toByteView("big"), toByteView(bigVal)));
            auto got = env.get(toByteView("big"));
            CHECK(got.ok());
            if (got.ok()) CHECK(bytesEqual(toByteView(got.value()), toByteView(bigVal)));
            auto st = env.stat();
            CHECK(st.ok());
            if (st.ok()) overflowPagesOn = st.value().overflowPages;
            CHECK_OK(env.close());
        }

        CHECK(overflowPagesOff > 0);
        // A 200000-byte run of a single repeated byte should compress to
        // well under one page's worth -- if this ever regresses to "no
        // savings", something in the write path stopped actually compressing.
        CHECK(overflowPagesOn < overflowPagesOff);
    }

    // ---- DUPSORT values unaffected by compression (aggressive minSize) ----
    {
        auto path = anntest::scratchDbPath("compression_dupsort.apdb");
        EnvironmentOptions opts;
        opts.enableCompression = true;
        opts.compressionMinSize = 1;  // as aggressive as possible
        auto envR = Environment::open(path, opts);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            if (!wtxnR) TEST_MAIN_END();
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            CHECK_OK(wtxn.putDup(toByteView("k"), toByteView("cccccccccccccccccccccccccccccc")));
            CHECK_OK(wtxn.putDup(toByteView("k"), toByteView("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")));
            CHECK_OK(wtxn.putDup(toByteView("k"), toByteView("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")));
            CHECK_OK(wtxn.commit());
        }
        {
            auto rtxnR = env.beginRead();
            CHECK(rtxnR.ok());
            if (!rtxnR) TEST_MAIN_END();
            txn::ReadTransaction rtxn = rtxnR.moveValue();
            btree::Cursor cur = rtxn.openCursor();
            auto seek = cur.seekExact(toByteView("k"));
            CHECK(seek.ok() && seek.value());
            auto first = cur.firstDup();
            CHECK(first.ok() && first.value());
            std::string values[3];
            for (int i = 0; i < 3; ++i) {
                // valueView() is the zero-copy path: it explicitly rejects
                // compressed cells, so a successful call here is itself
                // proof this dup value was never routed through compression.
                auto v = cur.valueView();
                CHECK(v.ok());
                if (v.ok()) values[i] = std::string(reinterpret_cast<const char*>(v.value().data()), v.value().size());
                if (i < 2) {
                    auto n = cur.nextDup();
                    CHECK(n.ok() && n.value());
                }
            }
            CHECK(values[0].size() == 30 && values[0][0] == 'a');
            CHECK(values[1].size() == 30 && values[1][0] == 'b');
            CHECK(values[2].size() == 30 && values[2][0] == 'c');
        }
        CHECK_OK(env.close());
    }

    // ---- DUPFIXED run unaffected by compression (aggressive minSize) ----
    {
        auto path = anntest::scratchDbPath("compression_dupfixed.apdb");
        EnvironmentOptions opts;
        opts.enableCompression = true;
        opts.compressionMinSize = 1;
        auto envR = Environment::open(path, opts);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            if (!wtxnR) TEST_MAIN_END();
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            btree::Cursor cur = wtxn.openCursor();
            cur.setFlags(DbFlags::DupFixed);
            constexpr std::uint32_t elemSize = 4;
            std::uint32_t elems[5] = {10, 20, 30, 40, 50};
            ByteBuffer blob(elemSize * 5);
            std::memcpy(blob.data(), elems, blob.size());
            CHECK_OK(cur.putMultiple(toByteView("fixed"), elemSize, toByteView(blob)));
            CHECK_OK(wtxn.commit());
        }
        {
            auto rtxnR = env.beginRead();
            CHECK(rtxnR.ok());
            if (!rtxnR) TEST_MAIN_END();
            txn::ReadTransaction rtxn = rtxnR.moveValue();
            btree::Cursor cur = rtxn.openCursor();
            cur.setFlags(DbFlags::DupFixed);
            auto seek = cur.seekExact(toByteView("fixed"));
            CHECK(seek.ok() && seek.value());
            auto first = cur.firstDup();
            CHECK(first.ok() && first.value());
            std::uint32_t expected[5] = {10, 20, 30, 40, 50};
            for (int i = 0; i < 5; ++i) {
                // Same reasoning as the DUPSORT case above: valueView()
                // succeeding proves this element was never compressed.
                auto v = cur.valueView();
                CHECK(v.ok());
                if (v.ok() && v.value().size() == 4) {
                    std::uint32_t got = 0;
                    std::memcpy(&got, v.value().data(), 4);
                    CHECK(got == expected[i]);
                }
                if (i < 4) {
                    auto n = cur.nextDup();
                    CHECK(n.ok() && n.value());
                }
            }
        }
        CHECK_OK(env.close());
    }

    TEST_MAIN_END();
}
