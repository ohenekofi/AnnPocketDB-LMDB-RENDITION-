#pragma once
//
// AnnPocketDb :: btree/db_flags.hpp
//
// Per-tree flags analogous to LMDB's mdb_dbi_open flags.
// Applied at operation time (and stored on SubDatabase / index metadata when
// those collections are opened with flags).
//
#include <cstdint>
#include <cstring>
#include <functional>

#include "annpocketdb/util/types.hpp"

namespace annpocketdb::btree {

enum class DbFlags : std::uint32_t {
    None = 0,
    // Allow multiple values per key, ordered by value (MDB_DUPSORT).
    DupSort = 1u << 0,
    // All duplicate values for a key have the same byte length (MDB_DUPFIXED).
    // Implies DupSort. Rejected on put if size mismatches an existing value.
    DupFixed = 1u << 1,
    // Keys compared from the end of the string toward the start (MDB_REVERSEKEY).
    // Useful for DNS-style suffixes and descending key scans.
    ReverseKey = 1u << 2,
    // Duplicate values compared from the end (MDB_REVERSEDUP). Implies DupSort.
    ReverseDup = 1u << 3,
    // Keys are native-endian unsigned integers of equal length (MDB_INTEGERKEY).
    // Typical sizes: 4 or 8 bytes. Mixed lengths fall back to memcmp of the
    // shorter prefix then length.
    IntegerKey = 1u << 4,
    // Dup values are native-endian unsigned integers (MDB_INTEGERDUP). Implies DupSort.
    IntegerDup = 1u << 5,
};

inline DbFlags operator|(DbFlags a, DbFlags b) {
    return static_cast<DbFlags>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
inline DbFlags operator&(DbFlags a, DbFlags b) {
    return static_cast<DbFlags>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
inline bool hasFlag(DbFlags set, DbFlags f) {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(f)) != 0;
}
inline DbFlags effectiveFlags(DbFlags f) {
    if (hasFlag(f, DbFlags::DupFixed) || hasFlag(f, DbFlags::ReverseDup) ||
        hasFlag(f, DbFlags::IntegerDup)) {
        f = f | DbFlags::DupSort;
    }
    return f;
}

// LMDB-style integer compare: same-length keys as native unsigned integers
// (platform endianness). Different lengths: compare common prefix then by length.
inline int compareIntegerBytes(ByteView a, ByteView b) {
    if (a.size() == b.size() && (a.size() == 4 || a.size() == 8)) {
        if (a.size() == 4) {
            std::uint32_t xa = 0, xb = 0;
            std::memcpy(&xa, a.data(), 4);
            std::memcpy(&xb, b.data(), 4);
            if (xa < xb) return -1;
            if (xa > xb) return 1;
            return 0;
        }
        std::uint64_t xa = 0, xb = 0;
        std::memcpy(&xa, a.data(), 8);
        std::memcpy(&xb, b.data(), 8);
        if (xa < xb) return -1;
        if (xa > xb) return 1;
        return 0;
    }
    // Variable / other sizes: big-endian style unsigned compare (prefix then length).
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
        auto ba = static_cast<unsigned char>(a[i]);
        auto bb = static_cast<unsigned char>(b[i]);
        if (ba < bb) return -1;
        if (ba > bb) return 1;
    }
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return 1;
    return 0;
}

// Compare from the last byte toward the first (LMDB REVERSEKEY / REVERSEDUP).
inline int compareBytesReverse(ByteView a, ByteView b) {
    std::size_t i = a.size();
    std::size_t j = b.size();
    while (i > 0 && j > 0) {
        --i;
        --j;
        const auto ba = static_cast<unsigned char>(a[i]);
        const auto bb = static_cast<unsigned char>(b[j]);
        if (ba < bb) return -1;
        if (ba > bb) return 1;
    }
    if (i == j) return 0;  // both zero
    // The side that still has a prefix (front of string) is "greater" in
    // reverse-order comparison only if the shared suffix matched — shorter
    // remaining front means that string ends first when reading forward from
    // the start; LMDB treats remaining length as determining order.
    return (i < j) ? -1 : 1;
}

struct KeyOrder {
    bool reverseKey = false;
    bool reverseDup = false;
    bool integerKey = false;
    bool integerDup = false;

    // Caller-supplied comparators (LMDB's mdb_set_compare/mdb_set_dupsort),
    // in-memory/per-process only -- never persisted, exactly like LMDB's own
    // custom comparators. When set, takes priority over every flag-driven
    // comparator below. See api::SubDatabaseManager::setKeyComparator/
    // setValueComparator for how a caller registers one per named database.
    std::function<int(ByteView, ByteView)> customKeyCmp;
    std::function<int(ByteView, ByteView)> customValueCmp;

    static KeyOrder fromFlags(DbFlags f) {
        f = effectiveFlags(f);
        KeyOrder o;
        o.reverseKey = hasFlag(f, DbFlags::ReverseKey);
        o.reverseDup = hasFlag(f, DbFlags::ReverseDup);
        o.integerKey = hasFlag(f, DbFlags::IntegerKey);
        o.integerDup = hasFlag(f, DbFlags::IntegerDup);
        return o;
    }

    int cmpKey(ByteView a, ByteView b) const {
        if (customKeyCmp) return customKeyCmp(a, b);
        int c;
        if (integerKey) c = compareIntegerBytes(a, b);
        else if (reverseKey) c = compareBytesReverse(a, b);
        else c = compareBytes(a, b);
        return c;
    }
    int cmpValue(ByteView a, ByteView b) const {
        if (customValueCmp) return customValueCmp(a, b);
        int c;
        if (integerDup) c = compareIntegerBytes(a, b);
        else if (reverseDup) c = compareBytesReverse(a, b);
        else c = compareBytes(a, b);
        return c;
    }
    // Full leaf entry order: key, then value (for DUPSORT).
    int cmpKeyValue(ByteView ka, ByteView va, ByteView kb, ByteView vb) const {
        int ck = cmpKey(ka, kb);
        if (ck != 0) return ck;
        return cmpValue(va, vb);
    }
};

}  // namespace annpocketdb::btree
