#include "annpocketdb/btree/node_codec.hpp"

#include <cstring>
#include <algorithm>

namespace annpocketdb::btree {

namespace {
constexpr std::size_t kSlotSize = 4;  // uint16 offset + uint16 length

std::uint16_t readU16(const std::byte* p) {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
void writeU16(std::byte* p, std::uint16_t v) { std::memcpy(p, &v, sizeof(v)); }

std::uint32_t readU32(const std::byte* p) {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
void writeU32(std::byte* p, std::uint32_t v) { std::memcpy(p, &v, sizeof(v)); }

std::uint64_t readU64(const std::byte* p) {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
void writeU64(std::byte* p, std::uint64_t v) { std::memcpy(p, &v, sizeof(v)); }
}  // namespace

// ---------------------------------------------------------------------------
// LeafPageView
// ---------------------------------------------------------------------------

LeafPageView::Slot LeafPageView::slotAt(std::uint16_t i) const {
    const std::byte* p = payload() + static_cast<std::size_t>(i) * kSlotSize;
    return Slot{readU16(p), readU16(p + 2)};
}

// Leaf cell: [u32 keyLen][u8 cellKind][u64 valueLenOrTotalLen][key][value|pageId|run]
// Fixed header before key = 4 + 1 + 8 = 13 bytes.
namespace {
constexpr std::size_t kLeafCellHdr = 13;  // keyLen + kind + valueLenOrTotalLen
constexpr std::uint8_t kCellInline = 0;
constexpr std::uint8_t kCellOverflow = 1;
constexpr std::uint8_t kCellFixedRun = 2;
constexpr std::uint8_t kCellSubTree = 3;
constexpr std::uint8_t kCellKindMask = 0x7Fu;
// Orthogonal flag on cellKind's otherwise-unused top bit -- see the format
// comment in node_codec.hpp. Only ever combined with kCellInline/kCellOverflow.
constexpr std::uint8_t kCompressedFlag = 0x80u;

std::uint8_t cellKindOf(const std::byte* cell) {
    return static_cast<std::uint8_t>(cell[4]) & kCellKindMask;
}
bool cellIsCompressed(const std::byte* cell) {
    return (static_cast<std::uint8_t>(cell[4]) & kCompressedFlag) != 0;
}
// Offset from `cell` to the start of the key bytes -- kLeafCellHdr, plus 8
// more when the compressed flag is set (room for uncompressedLen).
std::size_t cellKeyOffset(const std::byte* cell) {
    return kLeafCellHdr + (cellIsCompressed(cell) ? 8u : 0u);
}
}

ByteView LeafPageView::keyAt(std::uint16_t i) const {
    Slot s = slotAt(i);
    const std::byte* cell = payload() + s.offset;
    std::uint32_t keyLen = readU32(cell);
    return ByteView(cell + cellKeyOffset(cell), keyLen);
}

bool LeafPageView::isOverflowAt(std::uint16_t i) const {
    Slot s = slotAt(i);
    const std::byte* cell = payload() + s.offset;
    return cellKindOf(cell) == kCellOverflow;
}

bool LeafPageView::isFixedRunAt(std::uint16_t i) const {
    Slot s = slotAt(i);
    const std::byte* cell = payload() + s.offset;
    return cellKindOf(cell) == kCellFixedRun;
}

ByteView LeafPageView::inlineValueAt(std::uint16_t i) const {
    Slot s = slotAt(i);
    const std::byte* cell = payload() + s.offset;
    std::uint32_t keyLen = readU32(cell);
    std::uint64_t valueLen = readU64(cell + 5);
    return ByteView(cell + cellKeyOffset(cell) + keyLen, static_cast<std::size_t>(valueLen));
}

PageId LeafPageView::overflowFirstPageAt(std::uint16_t i) const {
    Slot s = slotAt(i);
    const std::byte* cell = payload() + s.offset;
    std::uint32_t keyLen = readU32(cell);
    return static_cast<PageId>(readU64(cell + cellKeyOffset(cell) + keyLen));
}

std::uint64_t LeafPageView::overflowTotalLenAt(std::uint16_t i) const {
    Slot s = slotAt(i);
    const std::byte* cell = payload() + s.offset;
    return readU64(cell + 5);
}

bool LeafPageView::isCompressedAt(std::uint16_t i) const {
    Slot s = slotAt(i);
    const std::byte* cell = payload() + s.offset;
    return cellIsCompressed(cell);
}

std::uint64_t LeafPageView::uncompressedLenAt(std::uint16_t i) const {
    Slot s = slotAt(i);
    const std::byte* cell = payload() + s.offset;
    return readU64(cell + 5 + 8);
}

std::uint32_t LeafPageView::fixedRunElemSizeAt(std::uint16_t i) const {
    Slot s = slotAt(i);
    const std::byte* cell = payload() + s.offset;
    std::uint32_t keyLen = readU32(cell);
    return readU32(cell + cellKeyOffset(cell) + keyLen);
}

std::uint32_t LeafPageView::fixedRunCountAt(std::uint16_t i) const {
    Slot s = slotAt(i);
    const std::byte* cell = payload() + s.offset;
    std::uint32_t keyLen = readU32(cell);
    return readU32(cell + cellKeyOffset(cell) + keyLen + 4);
}

ByteView LeafPageView::fixedRunBytesAt(std::uint16_t i) const {
    Slot s = slotAt(i);
    const std::byte* cell = payload() + s.offset;
    std::uint32_t keyLen = readU32(cell);
    std::uint64_t totalLen = readU64(cell + 5);  // count * elemSize
    return ByteView(cell + cellKeyOffset(cell) + keyLen + 8, static_cast<std::size_t>(totalLen));
}

bool LeafPageView::isSubTreeAt(std::uint16_t i) const {
    Slot s = slotAt(i);
    const std::byte* cell = payload() + s.offset;
    return cellKindOf(cell) == kCellSubTree;
}

PageId LeafPageView::subTreeRootAt(std::uint16_t i) const {
    Slot s = slotAt(i);
    const std::byte* cell = payload() + s.offset;
    std::uint32_t keyLen = readU32(cell);
    return static_cast<PageId>(readU64(cell + cellKeyOffset(cell) + keyLen));
}

std::uint16_t LeafPageView::lowerBound(ByteView key, const KeyOrder& order) const {
    std::uint16_t lo = 0, hi = count();
    while (lo < hi) {
        std::uint16_t mid = static_cast<std::uint16_t>(lo + (hi - lo) / 2);
        int c = order.cmpKey(keyAt(mid), key);
        if (c < 0) lo = static_cast<std::uint16_t>(mid + 1);
        else hi = mid;
    }
    return lo;
}

std::uint16_t LeafPageView::lowerBoundKeyValue(ByteView key, ByteView value, const KeyOrder& order) const {
    std::uint16_t lo = 0, hi = count();
    while (lo < hi) {
        std::uint16_t mid = static_cast<std::uint16_t>(lo + (hi - lo) / 2);
        int ck = order.cmpKey(keyAt(mid), key);
        int c = ck;
        if (ck == 0) {
            if (isOverflowAt(mid)) {
                c = 1;  // overflow sorts after inline dups
            } else {
                c = order.cmpValue(inlineValueAt(mid), value);
            }
        }
        if (c < 0) lo = static_cast<std::uint16_t>(mid + 1);
        else hi = mid;
    }
    return lo;
}

std::uint16_t LeafPageView::countDups(ByteView key) const {
    std::uint16_t i = lowerBound(key);
    std::uint16_t n = 0;
    while (i + n < count() && compareBytes(keyAt(static_cast<std::uint16_t>(i + n)), key) == 0) {
        ++n;
    }
    return n;
}

LeafEntry LeafPageView::entryAt(std::uint16_t i) const {
    LeafEntry e;
    e.key = toByteBuffer(keyAt(i));
    e.isOverflow = isOverflowAt(i);
    e.isCompressed = isCompressedAt(i);
    if (e.isCompressed) e.uncompressedLen = uncompressedLenAt(i);
    if (e.isOverflow) {
        e.overflowFirstPage = overflowFirstPageAt(i);
        e.overflowTotalLen = overflowTotalLenAt(i);
    } else if (isFixedRunAt(i)) {
        e.fixedElemSize = fixedRunElemSizeAt(i);
        e.value = toByteBuffer(fixedRunBytesAt(i));
    } else if (isSubTreeAt(i)) {
        e.isSubTree = true;
        e.subTreeRoot = subTreeRootAt(i);
    } else {
        e.value = toByteBuffer(inlineValueAt(i));
    }
    return e;
}

std::vector<LeafEntry> LeafPageView::decodeAll() const {
    std::vector<LeafEntry> out;
    out.reserve(count());
    for (std::uint16_t i = 0; i < count(); ++i) out.push_back(entryAt(i));
    return out;
}

std::size_t encodedLeafSize(const std::vector<LeafEntry>& entries) {
    std::size_t total = entries.size() * kSlotSize;
    for (const auto& e : entries) {
        total += kLeafCellHdr + (e.isCompressed ? 8 : 0) + e.key.size();
        if (e.isOverflow) {
            total += 8;
        } else if (e.isSubTree) {
            total += 8;  // just the sub-tree's root PageId
        } else if (e.fixedElemSize != 0) {
            total += 8 + e.value.size();  // elemSize + count headers, then the run bytes
        } else {
            total += e.value.size();
        }
    }
    return total;
}

bool encodeLeaf(PageHeader* header, std::uint32_t capacityBytes, const std::vector<LeafEntry>& entries) {
    const std::size_t used = encodedLeafSize(entries);
    if (used > capacityBytes) return false;
    std::byte* base = reinterpret_cast<std::byte*>(header) + sizeof(PageHeader);

    std::uint32_t cellEnd = capacityBytes;  // exclusive end of free cell area (shrinks)
    std::uint32_t slotCursor = 0;           // grows forward

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const LeafEntry& e = entries[i];
        const bool isFixedRun = !e.isOverflow && !e.isSubTree && e.fixedElemSize != 0;
        const std::uint32_t keyOffsetExtra = e.isCompressed ? 8u : 0u;
        std::uint32_t trailingLen = (e.isOverflow || e.isSubTree) ? 8u
                                     : isFixedRun ? static_cast<std::uint32_t>(8 + e.value.size())
                                                  : static_cast<std::uint32_t>(e.value.size());
        std::uint32_t cellLen = static_cast<std::uint32_t>(kLeafCellHdr + keyOffsetExtra + e.key.size() + trailingLen);
        cellEnd -= cellLen;
        std::byte* cell = base + cellEnd;
        writeU32(cell, static_cast<std::uint32_t>(e.key.size()));
        std::uint8_t kind = e.isOverflow  ? kCellOverflow
                            : e.isSubTree  ? kCellSubTree
                            : isFixedRun   ? kCellFixedRun
                                           : kCellInline;
        if (e.isCompressed) kind |= kCompressedFlag;
        cell[4] = static_cast<std::byte>(kind);
        if (e.isOverflow) {
            writeU64(cell + 5, e.overflowTotalLen);
        } else {
            writeU64(cell + 5, static_cast<std::uint64_t>(e.value.size()));
        }
        if (e.isCompressed) {
            writeU64(cell + 5 + 8, e.uncompressedLen);
        }
        std::byte* keyPtr = cell + kLeafCellHdr + keyOffsetExtra;
        std::memcpy(keyPtr, e.key.data(), e.key.size());
        if (e.isOverflow) {
            writeU64(keyPtr + e.key.size(), static_cast<std::uint64_t>(e.overflowFirstPage));
        } else if (e.isSubTree) {
            writeU64(keyPtr + e.key.size(), static_cast<std::uint64_t>(e.subTreeRoot));
        } else if (isFixedRun) {
            std::byte* runHdr = keyPtr + e.key.size();
            writeU32(runHdr, e.fixedElemSize);
            writeU32(runHdr + 4, e.fixedElemSize > 0
                                      ? static_cast<std::uint32_t>(e.value.size() / e.fixedElemSize)
                                      : 0u);
            if (!e.value.empty()) std::memcpy(runHdr + 8, e.value.data(), e.value.size());
        } else if (!e.value.empty()) {
            std::memcpy(keyPtr + e.key.size(), e.value.data(), e.value.size());
        }

        std::byte* slot = base + slotCursor;
        writeU16(slot, static_cast<std::uint16_t>(cellEnd));
        writeU16(slot + 2, static_cast<std::uint16_t>(cellLen));
        slotCursor += static_cast<std::uint32_t>(kSlotSize);
    }

    header->itemCount = static_cast<std::uint16_t>(entries.size());
    // Slots grow forward from offset 0; cells pack backward from capacityBytes.
    // The free gap between them is zeroed by allocatePage. CRC covers the full
    // capacity so it does not need to track the gap boundary (a few extra bytes
    // of CRC work vs. a more complex used-range calculation).
    (void)used;
    header->payloadSize = capacityBytes;
    return true;
}

// ---------------------------------------------------------------------------
// BranchPageView
// ---------------------------------------------------------------------------

BranchPageView::Slot BranchPageView::slotAt(std::uint16_t i) const {
    const std::byte* p = payload() + static_cast<std::size_t>(i) * kSlotSize;
    return Slot{readU16(p), readU16(p + 2)};
}

ByteView BranchPageView::keyAt(std::uint16_t i) const {
    Slot s = slotAt(i);
    const std::byte* cell = payload() + s.offset;
    std::uint32_t keyLen = readU32(cell);
    return ByteView(cell + 4 + 8, keyLen);
}

PageId BranchPageView::childAt(std::uint16_t i) const {
    Slot s = slotAt(i);
    const std::byte* cell = payload() + s.offset;
    return static_cast<PageId>(readU64(cell + 4));
}

std::uint16_t BranchPageView::findChildIndex(ByteView key, const KeyOrder& order) const {
    // Largest i such that keyAt(i) <= key. Entry 0's key is empty and is
    // treated as -infinity (always <=).
    std::uint16_t lo = 0, hi = count();
    while (lo < hi) {
        std::uint16_t mid = static_cast<std::uint16_t>(lo + (hi - lo) / 2);
        ByteView k = keyAt(mid);
        bool leOrZero = (mid == 0) || order.cmpKey(k, key) <= 0;
        if (leOrZero) lo = static_cast<std::uint16_t>(mid + 1);
        else hi = mid;
    }
    return lo == 0 ? 0 : static_cast<std::uint16_t>(lo - 1);
}

BranchEntry BranchPageView::entryAt(std::uint16_t i) const {
    BranchEntry e;
    e.key = toByteBuffer(keyAt(i));
    e.child = childAt(i);
    return e;
}

std::vector<BranchEntry> BranchPageView::decodeAll() const {
    std::vector<BranchEntry> out;
    out.reserve(count());
    for (std::uint16_t i = 0; i < count(); ++i) out.push_back(entryAt(i));
    return out;
}

std::size_t encodedBranchSize(const std::vector<BranchEntry>& entries) {
    std::size_t total = entries.size() * kSlotSize;
    for (const auto& e : entries) total += 4 + 8 + e.key.size();
    return total;
}

bool encodeBranch(PageHeader* header, std::uint32_t capacityBytes, const std::vector<BranchEntry>& entries) {
    if (encodedBranchSize(entries) > capacityBytes) return false;
    std::byte* base = reinterpret_cast<std::byte*>(header) + sizeof(PageHeader);

    std::uint32_t cellEnd = capacityBytes;
    std::uint32_t slotCursor = 0;

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const BranchEntry& e = entries[i];
        std::uint32_t cellLen = static_cast<std::uint32_t>(4 + 8 + e.key.size());
        cellEnd -= cellLen;
        std::byte* cell = base + cellEnd;
        writeU32(cell, static_cast<std::uint32_t>(e.key.size()));
        writeU64(cell + 4, static_cast<std::uint64_t>(e.child));
        std::memcpy(cell + 4 + 8, e.key.data(), e.key.size());

        std::byte* slot = base + slotCursor;
        writeU16(slot, static_cast<std::uint16_t>(cellEnd));
        writeU16(slot + 2, static_cast<std::uint16_t>(cellLen));
        slotCursor += static_cast<std::uint32_t>(kSlotSize);
    }

    header->itemCount = static_cast<std::uint16_t>(entries.size());
    header->payloadSize = capacityBytes;
    return true;
}

// ---------------------------------------------------------------------------
// Structural validation (ANNPOCKETDB_PARANOID)
// ---------------------------------------------------------------------------

Status validateNodePage(const PageHeader* header, std::uint32_t pageSize) {
    if (header == nullptr) return Status::Corruption("null page header");
    if (pageSize < sizeof(PageHeader)) {
        return Status::Corruption("pageSize smaller than PageHeader");
    }
    const std::uint32_t capacity =
        static_cast<std::uint32_t>(pageSize - sizeof(PageHeader));
    if (header->payloadSize > capacity) {
        return Status::Corruption("payloadSize exceeds page capacity");
    }
    const std::uint16_t n = header->itemCount;
    const std::size_t slotBytes = static_cast<std::size_t>(n) * kSlotSize;
    if (slotBytes > header->payloadSize) {
        return Status::Corruption("slot array exceeds payloadSize");
    }

    const std::byte* base =
        reinterpret_cast<const std::byte*>(header) + sizeof(PageHeader);
    const bool isLeaf = (header->type == core::PageType::Leaf);
    const bool isBranch = (header->type == core::PageType::Branch);
    if (!isLeaf && !isBranch) {
        return Status::Corruption("validateNodePage: not a leaf or branch");
    }

    for (std::uint16_t i = 0; i < n; ++i) {
        const std::byte* slotPtr = base + static_cast<std::size_t>(i) * kSlotSize;
        const std::uint16_t offset = readU16(slotPtr);
        const std::uint16_t length = readU16(slotPtr + 2);

        if (static_cast<std::uint32_t>(offset) + length > header->payloadSize) {
            return Status::Corruption("slot cell extends past payloadSize");
        }
        // Slot array occupies [0, slotBytes); cells pack from the end. A cell
        // must not overlap the slot array.
        if (offset < slotBytes) {
            return Status::Corruption("slot cell overlaps slot array");
        }
        if (length == 0) {
            return Status::Corruption("empty slot cell");
        }

        const std::byte* cell = base + offset;

        if (isLeaf) {
            // [u32 keyLen][u8 cellKind][u64 valueLenOrTotalLen][key][value|pageId|run]
            // (plus an extra [u64 uncompressedLen] before the key when the
            // compressed flag is set -- see node_codec.hpp's format comment.)
            if (length < kLeafCellHdr) {
                return Status::Corruption("leaf cell shorter than fixed header");
            }
            const std::uint32_t keyLen = readU32(cell);
            const std::uint8_t rawKind = static_cast<std::uint8_t>(cell[4]);
            const std::uint8_t cellKind = rawKind & kCellKindMask;
            const bool compressed = (rawKind & kCompressedFlag) != 0;
            const std::uint64_t valueLenOrTotal = readU64(cell + 5);

            if (compressed && cellKind != kCellInline && cellKind != kCellOverflow) {
                return Status::Corruption("leaf cell has compressed flag set on an unsupported cellKind");
            }
            std::size_t keyOffset = kLeafCellHdr;
            if (compressed) {
                if (kLeafCellHdr + 8 > length) {
                    return Status::Corruption("leaf cell too short for uncompressedLen field");
                }
                keyOffset += 8;
            }

            if (keyOffset + keyLen > length) {
                return Status::Corruption("leaf keyLen exceeds cell length");
            }
            if (cellKind == kCellOverflow) {
                if (keyOffset + keyLen + 8 > length) {
                    return Status::Corruption("leaf overflow pageId exceeds cell");
                }
            } else if (cellKind == kCellSubTree) {
                // Just an 8-byte root PageId; that page's own contents are
                // validated independently when it's actually read.
                if (keyOffset + keyLen + 8 > length) {
                    return Status::Corruption("leaf sub-tree root pageId exceeds cell");
                }
            } else if (cellKind == kCellFixedRun) {
                if (keyOffset + keyLen + 8 > length) {
                    return Status::Corruption("leaf fixed-run header exceeds cell length");
                }
                const std::uint32_t elemSize = readU32(cell + keyOffset + keyLen);
                const std::uint32_t runCount = readU32(cell + keyOffset + keyLen + 4);
                if (elemSize == 0) {
                    return Status::Corruption("leaf fixed-run elemSize is zero");
                }
                const std::uint64_t runBytes =
                    static_cast<std::uint64_t>(elemSize) * static_cast<std::uint64_t>(runCount);
                if (runBytes != valueLenOrTotal) {
                    return Status::Corruption("leaf fixed-run elemSize*count does not match valueLenOrTotal");
                }
                if (keyOffset + keyLen + 8 + static_cast<std::size_t>(runBytes) > length) {
                    return Status::Corruption("leaf fixed-run bytes exceed cell length");
                }
            } else if (cellKind == kCellInline) {
                if (keyOffset + keyLen + static_cast<std::size_t>(valueLenOrTotal) > length) {
                    return Status::Corruption("leaf valueLen exceeds cell length");
                }
            } else {
                return Status::Corruption("leaf cell has unknown cellKind");
            }
        } else {
            // Branch: [u32 keyLen][u64 childPageId][key bytes]
            constexpr std::size_t kBranchCellHdr = 4 + 8;
            if (length < kBranchCellHdr) {
                return Status::Corruption("branch cell shorter than fixed header");
            }
            const std::uint32_t keyLen = readU32(cell);
            if (kBranchCellHdr + keyLen > length) {
                return Status::Corruption("branch keyLen exceeds cell length");
            }
        }
    }
    return Status::OK();
}

}  // namespace annpocketdb::btree
