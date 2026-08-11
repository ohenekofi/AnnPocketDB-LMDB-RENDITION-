// Coverage for blob::BlobStore/BlobWriter/BlobReader (previously untested)
// plus Environment::putBlob/getBlob/removeBlob: single-page and multi-page
// (chunked, forced via a small pageSize) round-trips, BlobWriter's
// abandoned-without-finalize destructor safety net, BlobRef exact-byte-length
// boundary checks, a hand-crafted corrupt totalLen rejected without an
// oversized allocation attempt, and the Environment-level convenience-method
// edge cases (getBlob on a plain inline value, removeBlob likewise, blob
// chain reclamation on overwrite).
#include "test_common.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/blob/blob_store.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;
using namespace annpocketdb::blob;

static ByteBuffer makeData(std::size_t n, std::byte fill) {
    return ByteBuffer(n, fill);
}

int main() {
    // ---- small, single-page round-trip via BlobStore directly ----
    {
        auto path = anntest::scratchDbPath("test_blob_small.apdb");
        auto envR = Environment::open(path);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        BlobStore store(env.pageManager());
        BlobRef ref;
        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            ByteBuffer data = makeData(64, std::byte{'a'});
            auto putR = store.put(toByteView(data), wtxn.txnId());
            CHECK(putR.ok());
            if (putR.ok()) ref = putR.value();
            CHECK_OK(wtxn.commit());
        }
        CHECK(ref.totalLen == 64);
        auto getR = store.get(ref.id);
        CHECK(getR.ok());
        if (getR.ok()) {
            CHECK(getR.value().size() == 64);
            CHECK(getR.value()[0] == std::byte{'a'});
        }
        CHECK_OK(env.close());
    }

    // ---- multi-page chunked blob (small pageSize forces several overflow pages) ----
    {
        auto path = anntest::scratchDbPath("test_blob_chunked.apdb");
        EnvironmentOptions opts;
        opts.pageSize = core::kMinPageSize;  // 512 -> payloadCapacity ~474 bytes/page
        auto envR = Environment::open(path, opts);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        BlobStore store(env.pageManager());
        ByteBuffer data(1500);
        for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::byte>(i % 251);

        BlobId id = kInvalidBlobId;
        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            auto putR = store.put(toByteView(data), wtxn.txnId());
            CHECK(putR.ok());
            if (putR.ok()) id = putR.value().id;
            CHECK_OK(wtxn.commit());
        }

        // Whole-blob materialize.
        auto getR = store.get(id);
        CHECK(getR.ok());
        if (getR.ok()) CHECK(getR.value().size() == data.size() &&
                             std::memcmp(getR.value().data(), data.data(), data.size()) == 0);

        // Streaming reader, small reads to force crossing page boundaries.
        auto readerR = store.openReader(id);
        CHECK(readerR.ok());
        if (readerR.ok()) {
            BlobReader reader = readerR.moveValue();
            CHECK(reader.totalLength() == data.size());
            ByteBuffer got(data.size());
            std::size_t off = 0;
            while (off < got.size()) {
                std::size_t chunk = std::min<std::size_t>(137, got.size() - off);
                auto n = reader.read(got.data() + off, chunk);
                CHECK(n.ok());
                if (!n.ok() || n.value() == 0) break;
                off += static_cast<std::size_t>(n.value());
            }
            CHECK(off == data.size());
            CHECK(std::memcmp(got.data(), data.data(), data.size()) == 0);

            // seek beyond totalLength() is rejected.
            CHECK(!reader.seek(data.size() + 1).ok());
            // seek to a valid mid-point then read the remainder.
            CHECK_OK(reader.seek(1000));
            ByteBuffer tail(data.size() - 1000);
            auto tn = reader.read(tail);
            CHECK(tn.ok());
            if (tn.ok()) CHECK(static_cast<std::size_t>(tn.value()) == tail.size());
            CHECK(std::memcmp(tail.data(), data.data() + 1000, tail.size()) == 0);
        }
        CHECK_OK(env.close());
    }

    // ---- BlobWriter streaming + double-finalize + abandoned-without-finalize ----
    {
        auto path = anntest::scratchDbPath("test_blob_writer.apdb");
        EnvironmentOptions opts;
        opts.pageSize = core::kMinPageSize;
        auto envR = Environment::open(path, opts);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        // Multiple write() calls before one finalize().
        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            auto wR = BlobStore(env.pageManager()).openWriter(wtxn.txnId());
            CHECK(wR.ok());
            if (wR.ok()) {
                BlobWriter writer = wR.moveValue();
                std::string a(300, 'x'), b(300, 'y');
                CHECK_OK(writer.write(toByteView(a)));
                CHECK_OK(writer.write(toByteView(b)));
                CHECK(writer.bytesWritten() == 600);
                auto finR = writer.finalize();
                CHECK(finR.ok());
                CHECK(writer.finalized());
                // Second write()/finalize() after already-finalized fails.
                Status wsAfter = writer.write(toByteView(a));
                CHECK(!wsAfter.ok());
                CHECK(wsAfter.code() == StatusCode::Internal);
                auto finAgain = writer.finalize();
                CHECK(!finAgain.ok());
                CHECK(finAgain.status().code() == StatusCode::Internal);

                if (finR.ok()) {
                    auto getR = BlobStore(env.pageManager()).get(finR.value());
                    CHECK(getR.ok());
                    if (getR.ok()) CHECK(getR.value().size() == 600);
                }
            }
            CHECK_OK(wtxn.commit());
        }

        // Abandoned writer (write() then let it destruct without finalize()):
        // the pages it allocated must be retired, not leaked.
        {
            std::size_t retiredBefore = 0, reusableBefore = 0;
            {
                auto wtxnR = env.beginWrite();
                CHECK(wtxnR.ok());
                txn::WriteTransaction wtxn = wtxnR.moveValue();
                retiredBefore = env.pageManager().freeList().retiredCount();
                reusableBefore = env.pageManager().freeList().reusableCount();
                auto wR = BlobStore(env.pageManager()).openWriter(wtxn.txnId());
                CHECK(wR.ok());
                if (wR.ok()) {
                    BlobWriter writer = wR.moveValue();
                    std::string big(1500, 'z');
                    CHECK_OK(writer.write(toByteView(big)));
                    // writer destructs here without finalize() -- safety net should retire its pages.
                }
                CHECK_OK(wtxn.commit());
            }
            std::size_t retiredAfter = env.pageManager().freeList().retiredCount();
            std::size_t reusableAfter = env.pageManager().freeList().reusableCount();
            CHECK((retiredAfter + reusableAfter) > (retiredBefore + reusableBefore));
        }
        CHECK_OK(env.close());
    }

    // ---- BlobRef wire-format exact-boundary checks ----
    {
        BlobRef ref{42, 12345};
        ByteBuffer enc = encodeBlobRef(ref);
        CHECK(enc.size() == kBlobRefPrefix.size() + 1 + 8 + 8);
        CHECK(isBlobRef(toByteView(enc)));
        BlobRef decoded;
        CHECK_OK(decodeBlobRef(toByteView(enc), decoded));
        CHECK(decoded.id == 42 && decoded.totalLen == 12345);

        ByteBuffer tooShort(enc.begin(), enc.end() - 1);
        CHECK(!isBlobRef(toByteView(tooShort)));
        ByteBuffer tooLong = enc;
        tooLong.push_back(std::byte{0});
        CHECK(!isBlobRef(toByteView(tooLong)));
        ByteBuffer wrongPrefix = enc;
        wrongPrefix[0] = std::byte{'X'};
        CHECK(!isBlobRef(toByteView(wrongPrefix)));

        BlobRef dummy;
        Status bad = decodeBlobRef(toByteView(tooShort), dummy);
        CHECK(!bad.ok());
        CHECK(bad.code() == StatusCode::InvalidArgument);
    }

    // ---- corruption guard: a head page claiming totalLen past the map size is rejected ----
    {
        auto path = anntest::scratchDbPath("test_blob_corrupt.apdb");
        EnvironmentOptions opts;
        opts.maxDatabaseSize = 4ull << 20;  // small so mappedSize() is small/known
        auto envR = Environment::open(path, opts);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        BlobStore store(env.pageManager());
        BlobId headId = kInvalidBlobId;
        std::uint32_t origPayloadSize = 0;
        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            ByteBuffer data = makeData(32, std::byte{'q'});
            auto putR = store.put(toByteView(data), wtxn.txnId());
            CHECK(putR.ok());
            if (putR.ok()) headId = putR.value().id;

            auto pageR = env.pageManager().getPage(headId);
            CHECK(pageR.ok());
            if (pageR.ok()) origPayloadSize = pageR.value()->payloadSize;

            // writePageAt() zeroes the entire page before handing back a
            // writable pointer (see PageManager::writePageAt), so the magic
            // must be rewritten too, not just the totalLen field -- otherwise
            // this would trip the magic-mismatch check instead of the
            // intended oversized-totalLen check.
            auto mutR = env.pageManager().writePageAt(headId, wtxn.txnId(), core::PageType::Overflow);
            CHECK(mutR.ok());
            if (mutR.ok()) {
                core::PageHeader* header = mutR.value();
                std::byte* p = core::PageManager::payload(header);
                std::uint32_t magic = kBlobMagic;
                std::uint64_t hugeLen = env.pageManager().mappedFile().mappedSize() + (1ull << 30);
                std::memcpy(p, &magic, sizeof(magic));
                std::memcpy(p + 4, &hugeLen, sizeof(hugeLen));
                env.pageManager().finalizePage(header, origPayloadSize, /*itemCount=*/0);
            }
            CHECK_OK(wtxn.commit());
        }

        auto openR = BlobReader::open(env.pageManager(), headId);
        CHECK(!openR.ok());
        CHECK(openR.status().code() == StatusCode::Corruption);

        auto getR = store.get(headId);
        CHECK(!getR.ok());
        CHECK(getR.status().code() == StatusCode::Corruption);

        CHECK_OK(env.close());
    }

    // ---- Environment::putBlob/getBlob/removeBlob convenience-method edge cases ----
    {
        auto path = anntest::scratchDbPath("test_blob_env.apdb");
        auto envR = Environment::open(path);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        // getBlob on a plain (non-blob) inline value returns it unchanged.
        CHECK_OK(env.put(toByteView(std::string("plainkey")), toByteView(std::string("plainvalue"))));
        auto plainR = env.getBlob(toByteView(std::string("plainkey")));
        CHECK(plainR.ok());
        if (plainR.ok()) CHECK(toStringView(toByteView(plainR.value())) == "plainvalue");

        // getBlob on a missing key is NotFound.
        auto missingR = env.getBlob(toByteView(std::string("nosuchkey")));
        CHECK(!missingR.ok());
        CHECK(missingR.status().code() == StatusCode::NotFound);

        // removeBlob on a plain inline value still deletes the key.
        CHECK_OK(env.removeBlob(toByteView(std::string("plainkey"))));
        CHECK(!env.get(toByteView(std::string("plainkey"))).ok());

        // putBlob then re-putBlob a different-sized blob under the same key:
        // the old chain must be reclaimed, not leaked.
        std::string blobKey = "blobkey";
        ByteBuffer small = makeData(50, std::byte{'1'});
        ByteBuffer large = makeData(5000, std::byte{'2'});
        auto put1 = env.putBlob(toByteView(blobKey), toByteView(small));
        CHECK(put1.ok());
        std::size_t retiredBefore = env.pageManager().freeList().retiredCount();
        std::size_t reusableBefore = env.pageManager().freeList().reusableCount();

        auto put2 = env.putBlob(toByteView(blobKey), toByteView(large));
        CHECK(put2.ok());
        std::size_t retiredAfter = env.pageManager().freeList().retiredCount();
        std::size_t reusableAfter = env.pageManager().freeList().reusableCount();
        // The old (small) chain's page(s) must show up as retired/reusable,
        // not simply vanish unaccounted-for.
        CHECK((retiredAfter + reusableAfter) > (retiredBefore + reusableBefore));

        auto getLarge = env.getBlob(toByteView(blobKey));
        CHECK(getLarge.ok());
        if (getLarge.ok()) CHECK(getLarge.value().size() == large.size());

        // removeBlob on a real blob value retires its chain and deletes the key.
        CHECK_OK(env.removeBlob(toByteView(blobKey)));
        CHECK(!env.get(toByteView(blobKey)).ok());

        CHECK_OK(env.close());
    }

    TEST_MAIN_END();
}
