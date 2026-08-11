#include "annpocketdb/util/crc32c.hpp"

#include <array>
#include <cstring>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    #include <nmmintrin.h>
    #if defined(_MSC_VER)
        #include <intrin.h>
    #endif
    #define ANNPOCKETDB_CRC32C_X86 1
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
    #include <arm_acle.h>
    #define ANNPOCKETDB_CRC32C_ARM 1
#endif

namespace annpocketdb::util {
namespace {

constexpr std::array<std::uint32_t, 256> buildTable() {
    std::array<std::uint32_t, 256> table{};
    constexpr std::uint32_t poly = 0x82F63B78u;  // reversed CRC-32C (Castagnoli) polynomial
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (poly ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kTable = buildTable();

std::uint32_t softwareExtend(std::uint32_t crc, const std::uint8_t* p, std::size_t len) {
    crc = ~crc;
    for (std::size_t i = 0; i < len; ++i) {
        crc = kTable[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

#if defined(ANNPOCKETDB_CRC32C_X86)

bool hasSse42() {
#if defined(_MSC_VER)
    int info[4] = {0, 0, 0, 0};
    __cpuid(info, 1);
    return (info[2] & (1 << 20)) != 0;
#else
    unsigned int eax = 1, ebx = 0, ecx = 0, edx = 0;
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax));
    return (ecx & (1u << 20)) != 0;
#endif
}

std::uint32_t hardwareExtend(std::uint32_t crc, const std::uint8_t* p, std::size_t len) {
    crc = ~crc;
    while (len && (reinterpret_cast<std::uintptr_t>(p) & 7u)) {
        crc = _mm_crc32_u8(crc, *p);
        ++p; --len;
    }
#if defined(_M_X64) || defined(__x86_64__)
    while (len >= 8) {
        std::uint64_t v;
        std::memcpy(&v, p, sizeof(v));
        crc = static_cast<std::uint32_t>(_mm_crc32_u64(crc, v));
        p += 8; len -= 8;
    }
#endif
    while (len >= 4) {
        std::uint32_t v;
        std::memcpy(&v, p, sizeof(v));
        crc = _mm_crc32_u32(crc, v);
        p += 4; len -= 4;
    }
    while (len) {
        crc = _mm_crc32_u8(crc, *p);
        ++p; --len;
    }
    return ~crc;
}

#elif defined(ANNPOCKETDB_CRC32C_ARM)

std::uint32_t hardwareExtend(std::uint32_t crc, const std::uint8_t* p, std::size_t len) {
    crc = ~crc;
    while (len >= 8) {
        std::uint64_t v;
        std::memcpy(&v, p, sizeof(v));
        crc = __crc32cd(crc, v);
        p += 8; len -= 8;
    }
    while (len) {
        crc = __crc32cb(crc, *p);
        ++p; --len;
    }
    return ~crc;
}

#endif

}  // namespace

std::uint32_t crc32cExtend(std::uint32_t crc, ByteView data) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    const std::size_t len = data.size();

#if defined(ANNPOCKETDB_CRC32C_X86)
    static const bool kHwAvailable = hasSse42();
    if (kHwAvailable) return hardwareExtend(crc, p, len);
#elif defined(ANNPOCKETDB_CRC32C_ARM)
    return hardwareExtend(crc, p, len);
#endif

    return softwareExtend(crc, p, len);
}

std::uint32_t crc32c(std::uint32_t initial, const void* data, std::size_t len) {
    return crc32cExtend(initial, ByteView(reinterpret_cast<const std::byte*>(data), len));
}

std::uint32_t maskCrc32c(std::uint32_t crc) {
    constexpr std::uint32_t kMaskDelta = 0xa282ead8u;
    return ((crc >> 15) | (crc << 17)) + kMaskDelta;
}

std::uint32_t unmaskCrc32c(std::uint32_t maskedCrc) {
    constexpr std::uint32_t kMaskDelta = 0xa282ead8u;
    const std::uint32_t rot = maskedCrc - kMaskDelta;
    return (rot >> 17) | (rot << 15);
}

}  // namespace annpocketdb::util
