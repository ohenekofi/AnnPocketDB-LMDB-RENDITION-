// CRC32C (Castagnoli) checksum smoke test: the standard check-value test
// vector, chained (multi-buffer) computation matching one-shot computation,
// and the LevelDB/RocksDB-style mask/unmask round trip used to keep an
// on-disk CRC field from plausibly colliding with the buffer it protects.
#include "test_common.hpp"

#include <cstring>
#include <vector>

#include "annpocketdb/util/crc32c.hpp"

using namespace annpocketdb;

int main() {
    // Standard CRC32C (Castagnoli) check value for the ASCII string
    // "123456789" -- the canonical test vector used by every CRC32C
    // implementation (RFC 3720 / iSCSI, and the value SSE4.2's CRC32
    // instruction and the ARMv8 CRC extension are both validated against).
    {
        const char* data = "123456789";
        std::uint32_t crc = util::crc32c(0, data, std::strlen(data));
        CHECK(crc == 0xE3069283u);
    }

    // Chained (incremental) computation across two buffers must produce the
    // exact same result as a single one-shot call over the concatenation --
    // this is what PageManager::computePageCrc relies on (header, then
    // payload, as two separate crc32c/crc32cExtend calls).
    {
        const char* data = "The quick brown fox jumps over the lazy dog";
        std::size_t len = std::strlen(data);
        std::uint32_t oneShot = util::crc32c(0, data, len);

        std::size_t split = len / 2;
        std::uint32_t chained = util::crc32c(0, data, split);
        ByteView rest(reinterpret_cast<const std::byte*>(data) + split, len - split);
        chained = util::crc32cExtend(chained, rest);
        CHECK(chained == oneShot);

        // Splitting at a different point must still agree.
        std::size_t split2 = 3;
        std::uint32_t chained2 = util::crc32c(0, data, split2);
        ByteView rest2(reinterpret_cast<const std::byte*>(data) + split2, len - split2);
        chained2 = util::crc32cExtend(chained2, rest2);
        CHECK(chained2 == oneShot);
    }

    // Different inputs must produce different digests -- a basic sensitivity
    // check (not a substitute for a full avalanche/statistical test, just a
    // smoke check that this isn't secretly a constant or a trivial identity).
    {
        std::uint32_t a = util::crc32c(0, "abc", 3);
        std::uint32_t b = util::crc32c(0, "abd", 3);
        std::uint32_t c = util::crc32c(0, "abcd", 4);
        CHECK(a != b);
        CHECK(a != c);
    }

    // A single-bit flip anywhere in the buffer must change the digest --
    // this is the actual property PageManager::getPage relies on to detect
    // page corruption at all.
    {
        std::vector<std::uint8_t> buf(256);
        for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<std::uint8_t>(i * 31 + 7);
        std::uint32_t original = util::crc32c(0, buf.data(), buf.size());

        for (std::size_t bitPos : {0u, 1u, 63u, 127u, 255u * 8u + 7u}) {
            std::vector<std::uint8_t> flipped = buf;
            std::size_t byteIdx = bitPos / 8;
            if (byteIdx >= flipped.size()) continue;
            flipped[byteIdx] ^= static_cast<std::uint8_t>(1u << (bitPos % 8));
            std::uint32_t crc = util::crc32c(0, flipped.data(), flipped.size());
            CHECK(crc != original);
        }
    }

    // mask/unmask round trip: masking then unmasking recovers the original
    // value exactly, and (for a typical non-zero input) the masked form
    // differs from the raw crc -- sanity that masking actually transforms
    // the bits rather than being a no-op.
    {
        const char* data = "annpocketdb";
        std::uint32_t crc = util::crc32c(0, data, std::strlen(data));
        std::uint32_t masked = util::maskCrc32c(crc);
        CHECK(masked != crc);
        std::uint32_t unmasked = util::unmaskCrc32c(masked);
        CHECK(unmasked == crc);
    }

    // Masking an all-zero buffer's crc must not itself collide with the
    // literal value 0 -- the entire point of masking is to avoid an on-disk
    // checksum field aliasing a trivially-producible bit pattern (e.g. a
    // freshly-zeroed page whose "checksum" happens to also read as zero).
    {
        std::vector<std::byte> zeros(64, std::byte{0});
        std::uint32_t crc = util::crc32c(0, zeros.data(), zeros.size());
        std::uint32_t masked = util::maskCrc32c(crc);
        CHECK(masked != 0);
        CHECK(util::unmaskCrc32c(masked) == crc);
    }

    // Mask/unmask round trip must hold across a range of inputs, not just
    // one lucky value.
    {
        for (std::uint32_t seed = 0; seed < 50; ++seed) {
            std::vector<std::uint8_t> buf(seed + 1);
            for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<std::uint8_t>((seed * 17 + i) & 0xFF);
            std::uint32_t crc = util::crc32c(0, buf.data(), buf.size());
            CHECK(util::unmaskCrc32c(util::maskCrc32c(crc)) == crc);
        }
    }

    TEST_MAIN_END();
}