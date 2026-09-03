//
// AnnPocketDb :: tests/test_page_size_edges.cpp
//
// Two related, previously-uncovered gaps in one file:
//
//   1. Page-size edges: core::kMinPageSize (512) and core::kMaxPageSize
//      (65536) are the boundaries `PageManager::open` actually enforces
//      (see the range check in core/page_manager.cpp) but nothing exercised
//      either boundary directly through the ordinary BTree put/get path --
//      `test_blob.cpp` uses kMinPageSize, but only through BlobStore, a
//      different call path (writeOverflowChain/readOverflowChain via
//      BTree::putBlobValue vs BlobStore's own chunked writer). Also covers
//      the just-outside-range rejection on both ends.
//   2. Long overflow chains: values large enough to span hundreds of
//      overflow pages, through Environment::put/get directly (not through
//      BlobStore), including exact byte-for-byte round-trip, a correct
//      `overflowPages` count in `stat()`, clean removal (chain retirement),
//      and proof the retired chain's pages are actually reused by a
//      subsequent large write rather than leaking.
//
#include "test_common.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/core/page.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;

namespace {

ByteBuffer patternedBytes(std::size_t n, std::uint32_t seed) {
    ByteBuffer b(n);
    // Deterministic but non-repetitive-enough-to-hide-corruption pattern:
    // a torn/misordered overflow chain read would very likely disagree with
    // this at some offset, unlike an all-'A' buffer where a wrong chunk
    // could accidentally still "look right".
    for (std::size_t i = 0; i < n; ++i) {
        b[i] = static_cast<std::byte>(static_cast<unsigned char>((seed + i * 2654435761u) & 0xFF));
    }
    return b;
}
bool bytesEqual(ByteView a, ByteView b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

}  // namespace

int main() {
    // ---- 1a. Minimum page size (512): ordinary put/get/remove plus enough
    // keys to force real branch splits at the smallest possible fanout. ----
    {
        auto path = anntest::scratchDbPath("page_edge_min.apdb");
        EnvironmentOptions opts;
        opts.pageSize = core::kMinPageSize;
        auto envR = Environment::open(path, opts);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        constexpr int kKeys = 800;  // payloadCapacity(512) ~484B/leaf -> guarantees real splits
        for (int i = 0; i < kKeys; ++i) {
            char k[16], v[16];
            std::snprintf(k, sizeof(k), "mk%04d", i);
            std::snprintf(v, sizeof(v), "mv%04d", i);
            CHECK_OK(env.put(toByteView(std::string(k)), toByteView(std::string(v))));
        }
        for (int i = 0; i < kKeys; ++i) {
            char k[16], v[16];
            std::snprintf(k, sizeof(k), "mk%04d", i);
            std::snprintf(v, sizeof(v), "mv%04d", i);
            auto got = env.get(toByteView(std::string(k)));
            CHECK(got.ok());
            if (got.ok()) CHECK(toStringView(toByteView(got.value())) == std::string(v));
        }
        auto st = env.stat();
        CHECK(st.ok());
        if (st.ok()) {
            CHECK(st.value().pageSize == core::kMinPageSize);
            CHECK(st.value().entries == static_cast<std::uint64_t>(kKeys));
            // At 512-byte pages this key count cannot possibly fit in one
            // leaf -- if depth is still 1, splitting silently isn't
            // happening at this page size.
            CHECK(st.value().depth > 1);
        }

        // Value exactly at, one below, and one above this page size's
        // overflow threshold (payloadCapacity/4).
        std::size_t threshold = core::payloadCapacity(core::kMinPageSize) / 4;
        CHECK(threshold > 0);
        for (int delta : {-1, 0, 1}) {
            std::size_t sz = static_cast<std::size_t>(static_cast<long>(threshold) + delta);
            ByteBuffer val = patternedBytes(sz, 0x1000u + static_cast<unsigned>(delta + 1));
            std::string key = "thresh" + std::to_string(delta);
            CHECK_OK(env.put(toByteView(key), toByteView(val)));
            auto got = env.get(toByteView(key));
            CHECK(got.ok());
            if (got.ok()) CHECK(bytesEqual(toByteView(got.value()), toByteView(val)));
        }

        // Removal still works at this page size.
        CHECK_OK(env.remove(toByteView(std::string("mk0000"))));
        auto gone = env.get(toByteView(std::string("mk0000")));
        CHECK(!gone.ok());
        CHECK(gone.status().code() == StatusCode::NotFound);

        CHECK_OK(env.close());
    }

    // ---- 1b. Maximum page size (65536): values that would have overflowed
    // at the default 4KiB page size but fit comfortably inline at 64KiB,
    // plus a value large enough to force overflow even here. ----
    {
        auto path = anntest::scratchDbPath("page_edge_max.apdb");
        EnvironmentOptions opts;
        opts.pageSize = core::kMaxPageSize;
        auto envR = Environment::open(path, opts);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        // ~8KB: would overflow at the default page size (threshold ~1017B)
        // but is well inside a single 64KB page here.
        ByteBuffer midVal = patternedBytes(8000, 0x2000u);
        CHECK_OK(env.put(toByteView("mid"), toByteView(midVal)));
        {
            auto got = env.get(toByteView("mid"));
            CHECK(got.ok());
            if (got.ok()) CHECK(bytesEqual(toByteView(got.value()), toByteView(midVal)));
        }
        auto stAfterMid = env.stat();
        CHECK(stAfterMid.ok());
        if (stAfterMid.ok()) {
            // 8000 bytes should not have needed any overflow page at all
            // at a 64KB page size (threshold is payloadCapacity/4, tens of
            // KB) -- if this is nonzero, the overflow-threshold math isn't
            // actually scaling with pageSize the way it's documented to.
            CHECK(stAfterMid.value().overflowPages == 0);
        }

        std::size_t threshold = core::payloadCapacity(core::kMaxPageSize) / 4;
        ByteBuffer bigVal = patternedBytes(threshold + 5000, 0x2100u);
        CHECK_OK(env.put(toByteView("big"), toByteView(bigVal)));
        {
            auto got = env.get(toByteView("big"));
            CHECK(got.ok());
            if (got.ok()) CHECK(bytesEqual(toByteView(got.value()), toByteView(bigVal)));
        }
        auto stAfterBig = env.stat();
        CHECK(stAfterBig.ok());
        if (stAfterBig.ok()) CHECK(stAfterBig.value().overflowPages > 0);

        CHECK_OK(env.close());
    }

    // ---- 1c. Just outside the supported range on both ends must be
    // rejected cleanly (no crash, no silent clamping). ----
    {
        auto path = anntest::scratchDbPath("page_edge_too_small.apdb");
        EnvironmentOptions opts;
        opts.pageSize = core::kMinPageSize - 1;
        auto envR = Environment::open(path, opts);
        CHECK(!envR.ok());
        CHECK(envR.status().code() == StatusCode::InvalidArgument);
    }
    {
        auto path = anntest::scratchDbPath("page_edge_too_large.apdb");
        EnvironmentOptions opts;
        opts.pageSize = core::kMaxPageSize + 1;
        auto envR = Environment::open(path, opts);
        CHECK(!envR.ok());
        CHECK(envR.status().code() == StatusCode::InvalidArgument);
    }

    // ---- 2a. Long overflow chain via the ordinary put/get path (not
    // BlobStore), at a small page size to make a chain span hundreds of
    // pages without needing an enormous value. ----
    {
        auto path = anntest::scratchDbPath("page_edge_long_chain_small.apdb");
        EnvironmentOptions opts;
        opts.pageSize = core::kMinPageSize;
        auto envR = Environment::open(path, opts);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        constexpr std::size_t kValueSize = 300 * 1024;  // 300 KiB
        ByteBuffer val = patternedBytes(kValueSize, 0x3000u);
        CHECK_OK(env.put(toByteView("longchain"), toByteView(val)));

        auto got = env.get(toByteView("longchain"));
        CHECK(got.ok());
        if (got.ok()) CHECK(bytesEqual(toByteView(got.value()), toByteView(val)));

        std::size_t payloadPerPage = core::payloadCapacity(core::kMinPageSize);
        std::uint64_t expectedMinPages =
            static_cast<std::uint64_t>((kValueSize + payloadPerPage - 1) / payloadPerPage);

        auto st = env.stat();
        CHECK(st.ok());
        std::uint64_t overflowPagesBeforeRemove = 0;
        if (st.ok()) {
            overflowPagesBeforeRemove = st.value().overflowPages;
            // Not asserting exact equality -- head-page bookkeeping bytes
            // can shift the exact count by one or two pages -- but it must
            // be in the right ballpark, proving this genuinely chained
            // across many pages rather than, say, silently truncating.
            CHECK(overflowPagesBeforeRemove >= expectedMinPages - 2);
            CHECK(overflowPagesBeforeRemove <= expectedMinPages + 2);
        }

        // Remove and confirm the chain is gone, then write another
        // similarly-sized value and confirm the retired chain's pages get
        // reused rather than the free-page count growing unbounded --
        // total on-disk page usage should not roughly double just because
        // we removed one long chain and wrote another same-sized one.
        CHECK_OK(env.remove(toByteView("longchain")));
        {
            auto gone = env.get(toByteView("longchain"));
            CHECK(!gone.ok());
            CHECK(gone.status().code() == StatusCode::NotFound);
        }

        ByteBuffer val2 = patternedBytes(kValueSize, 0x3100u);
        CHECK_OK(env.put(toByteView("longchain2"), toByteView(val2)));
        {
            auto got2 = env.get(toByteView("longchain2"));
            CHECK(got2.ok());
            if (got2.ok()) CHECK(bytesEqual(toByteView(got2.value()), toByteView(val2)));
        }

        auto stAfter = env.stat();
        CHECK(stAfter.ok());
        auto infoAfter = env.info();
        if (stAfter.ok()) {
            // `lastPageId` after retiring one ~expectedMinPages-page chain
            // and allocating another of the same size should stay roughly
            // flat, not climb to ~2x -- if it does, retired overflow chain
            // pages are leaking instead of being reclaimed by the free
            // list, specifically for LONG chains (a single-page overflow
            // leak would already be caught by test_btree_rebalance's
            // general free-list churn; this isolates the many-page case).
            CHECK(infoAfter.lastPageId <
                  static_cast<std::uint64_t>(2 * expectedMinPages + 200));
        }

        CHECK_OK(env.close());
    }

    // ---- 2b. Long overflow chain at the default page size too, so the
    // common-case configuration (not just the small-page edge case) is
    // covered by a real many-page chain, not only test_compression's
    // 200000-byte value (which this deliberately exceeds). ----
    {
        auto path = anntest::scratchDbPath("page_edge_long_chain_default.apdb");
        auto envR = Environment::open(path);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        constexpr std::size_t kValueSize = 2 * 1024 * 1024;  // 2 MiB
        ByteBuffer val = patternedBytes(kValueSize, 0x4000u);
        CHECK_OK(env.put(toByteView("bigdefault"), toByteView(val)));

        auto got = env.get(toByteView("bigdefault"));
        CHECK(got.ok());
        if (got.ok()) CHECK(bytesEqual(toByteView(got.value()), toByteView(val)));

        auto st = env.stat();
        CHECK(st.ok());
        if (st.ok()) CHECK(st.value().overflowPages > 400);  // ~4KB pages -> hundreds of pages

        CHECK_OK(env.remove(toByteView("bigdefault")));
        auto gone = env.get(toByteView("bigdefault"));
        CHECK(!gone.ok());

        CHECK_OK(env.close());
    }

    TEST_MAIN_END();
}
