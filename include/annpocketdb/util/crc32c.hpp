#pragma once
//
// AnnPocketDb :: util/crc32c.hpp
//
// CRC32C (Castagnoli) checksum, used to validate every page and every WAL
// record. Hardware-accelerated via SSE4.2 (x86/x64) or the ARMv8 CRC
// extension when available, detected at runtime and falling back to a
// portable table-based software implementation otherwise.
//
#include <cstddef>
#include <cstdint>

#include "annpocketdb/util/types.hpp"

namespace annpocketdb::util {

// One-shot: crc32c(0, data, len) computes the CRC32C of a single buffer.
std::uint32_t crc32c(std::uint32_t initial, const void* data, std::size_t len);

// Incremental: continues a computation across multiple buffers, e.g.
//   std::uint32_t crc = util::crc32c(0, &header, sizeof(header));
//   crc = util::crc32cExtend(crc, payload);
std::uint32_t crc32cExtend(std::uint32_t crc, ByteView data);

// LevelDB/RocksDB-style masking so a CRC field's bit pattern never collides
// with what the protected buffer itself would produce for pathological
// inputs (e.g. an all-zero record aliasing its own checksum field).
std::uint32_t maskCrc32c(std::uint32_t crc);
std::uint32_t unmaskCrc32c(std::uint32_t maskedCrc);

}  // namespace annpocketdb::util
