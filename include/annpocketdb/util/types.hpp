#pragma once
// AnnPocketDb :: util/types.hpp
// Fundamental aliases shared by every layer.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace annpocketdb {

using PageId = std::uint64_t;
constexpr PageId kInvalidPageId = ~PageId(0);

using TxnId = std::uint64_t;
constexpr TxnId kInvalidTxnId = 0;

// Integer handle for a named sub-database (LMDB's MDB_dbi). 0 is reserved/invalid.
using SubDbHandle = std::uint32_t;
constexpr SubDbHandle kInvalidSubDbHandle = 0;

using SequenceNumber = std::uint64_t;

using ByteView = std::span<const std::byte>;
using ByteBuffer = std::vector<std::byte>;

inline ByteView toByteView(std::string_view s) {
    return ByteView(reinterpret_cast<const std::byte*>(s.data()), s.size());
}
inline ByteView toByteView(const ByteBuffer& b) { return ByteView(b.data(), b.size()); }
inline std::string_view toStringView(ByteView v) {
    return std::string_view(reinterpret_cast<const char*>(v.data()), v.size());
}
inline ByteBuffer toByteBuffer(ByteView v) { return ByteBuffer(v.begin(), v.end()); }
inline ByteBuffer toByteBuffer(std::string_view s) {
    ByteView v = toByteView(s);
    return ByteBuffer(v.begin(), v.end());
}

inline int compareBytes(ByteView a, ByteView b) {
    const std::size_t n = std::min(a.size(), b.size());
    if (n) {
        int c = std::memcmp(a.data(), b.data(), n);
        if (c != 0) return c < 0 ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

}  // namespace annpocketdb
