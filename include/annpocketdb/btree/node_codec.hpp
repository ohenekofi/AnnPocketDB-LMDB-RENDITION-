#pragma once
//
// AnnPocketDb :: btree/node_codec.hpp
//
// Slotted-page encoding for B+tree leaf and branch nodes, the on-disk heart
// of the engine.
//
// Layout of a node's payload area:
//
//   [ slot array: itemCount x {uint16 cellOffset, uint16 cellLength} ]
//   [ ... free space ... ]
//   [ cell data, packed from the end of the payload backward ]
//
// Slots are kept in ascending key order, so lookups are a binary search
// over the slot array followed by an O(1) cell dereference -- no decoding
// of unrelated entries is needed for reads.
//
// Because every modification happens under copy-on-write (a changed node
// is always written to a brand-new, freshly-zeroed page), the *write* path
// does not need to reason about in-place fragmentation at all: it decodes
// the full entry set into memory, applies the edit, and re-encodes the
// page from scratch, compacted. This trades a small amount of CPU on
// writes (already paying for a page copy anyway under COW) for a much
// simpler and more auditable implementation.
//
// Leaf cell format (format version 1):
//   [uint32 keyLen][uint8 cellKind][uint64 valueLenOrTotalLen]
//   [key bytes]
//   [ if cellKind == Inline:   value bytes ]
//   [ if cellKind == Overflow: uint64 firstOverflowPageId ]
//   [ if cellKind == FixedRun: uint32 elemSize][uint32 count][count*elemSize bytes ]
//
// cellKind: 0 = Inline (single value), 1 = Overflow (blob chain),
// 2 = FixedRun (DUPFIXED: one physically-contiguous fixed-stride run of
// same-size values for this key, LMDB's MDB_MULTIPLE storage -- see
// BTree::insertMultiple / Cursor::putMultiple). A FixedRun cell is one leaf
// entry representing an entire dup-group at once; it cannot coexist with
// ordinary per-value dup cells for the same key (rejected as
// InvalidArgument at insert time).
// 3 = SubTree (DUPSORT escalation, LMDB's nested-tree dup storage): once an
// ordinary key's flat dup-cell-set grows past roughly half a page, its dup
// values are moved into their own embedded B+tree (same leaf/branch page
// format, recursively -- the sub-tree's "keys" ARE the original dup values;
// its "values" are unused/empty) and this one leaf entry just stores that
// sub-tree's root PageId. See BTree::insertRecursive's escalation step and
// BTree::collectSubTreeValues. De-escalation (shrinking back to flat
// storage) is not implemented -- a permanently-escalated group is correct,
// just not maximally compact if it later shrinks a lot.
//
// valueLenOrTotalLen is uint64 so overflow (blob) values are not capped at
// 4 GiB. Inline values (and FixedRun runs) are still constrained by page
// payload capacity; for FixedRun it holds count*elemSize (the total run
// byte length), matching Inline's "total value bytes" convention.
//
// Compression (value-level, see compression/lz4_codec.hpp and
// BTree::maybeCompress): cellKind's otherwise-unused top bit (0x80) is an
// orthogonal flag, valid only when the low bits are Inline or Overflow --
// never FixedRun/SubTree, which have structural requirements (uniform
// element stride; byte-comparable dup sort order) compression would break.
// When set, one extra [uint64 uncompressedLen] field is inserted
// immediately after valueLenOrTotalLen, before the key bytes; `value`/
// `overflowTotalLen`/`valueLenOrTotalLen` then describe the *compressed*
// bytes actually stored. Purely additive: a cell with the flag bit unset
// decodes byte-for-byte identically to the format described above, so no
// migration is needed for data written before compression existed.
//
// Branch cell format:
//   [uint32 keyLen][uint64 childPageId][key bytes]
// Entry 0 always has an empty key, representing "-infinity": child 0 covers
// every key less than entry 1's key.
//
// encodeLeaf / encodeBranch set header->payloadSize = capacityBytes (full
// payload). Slots and cells are non-contiguous (gap in the middle); CRC'ing
// the whole capacity avoids tracking the gap and is correct because the gap
// is always zeroed on allocate.
//
#include <cstdint>
#include <vector>
#include <optional>

#include "annpocketdb/core/page.hpp"
#include "annpocketdb/util/types.hpp"
#include "annpocketdb/btree/db_flags.hpp"
#include "annpocketdb/util/status.hpp"

namespace annpocketdb::btree {

using core::PageHeader;

// ---------------------------------------------------------------------------
// Leaf entries
// ---------------------------------------------------------------------------

struct LeafEntry {
    ByteBuffer key;
    bool isOverflow = false;
    ByteBuffer value;              // valid iff !isOverflow (packed run bytes when fixedElemSize != 0)
    PageId overflowFirstPage = kInvalidPageId;  // valid iff isOverflow
    std::uint64_t overflowTotalLen = 0;         // valid iff isOverflow
    // DUPFIXED fixed-stride run (mutually exclusive with isOverflow): 0
    // means an ordinary single inline value; non-zero means `value` holds
    // a contiguous run of (value.size() / fixedElemSize) same-size elements.
    std::uint32_t fixedElemSize = 0;
    // DUPSORT escalation (mutually exclusive with isOverflow/fixedElemSize):
    // when set, this key's dup values live in the sub-tree rooted here
    // instead of in `value`.
    bool isSubTree = false;
    PageId subTreeRoot = kInvalidPageId;
    // Value-level compression (valid only when !isSubTree && fixedElemSize
    // == 0; may combine with isOverflow). When set, `value`/`overflowTotalLen`
    // hold the *compressed* bytes/length, and `uncompressedLen` is the
    // original byte length needed to decompress back to the real value.
    bool isCompressed = false;
    std::uint64_t uncompressedLen = 0;
};

// Read-only, zero-copy view over an already-encoded leaf page.
class LeafPageView {
public:
    explicit LeafPageView(const PageHeader* header) : header_(header) {}

    std::uint16_t count() const { return header_->itemCount; }
    ByteView keyAt(std::uint16_t i) const;
    bool isOverflowAt(std::uint16_t i) const;
    ByteView inlineValueAt(std::uint16_t i) const;      // only if cellKind == Inline
    PageId overflowFirstPageAt(std::uint16_t i) const;  // only if isOverflowAt(i)
    std::uint64_t overflowTotalLenAt(std::uint16_t i) const;

    // DUPFIXED fixed-stride run accessors -- only valid when isFixedRunAt(i).
    bool isFixedRunAt(std::uint16_t i) const;
    std::uint32_t fixedRunElemSizeAt(std::uint16_t i) const;
    std::uint32_t fixedRunCountAt(std::uint16_t i) const;
    ByteView fixedRunBytesAt(std::uint16_t i) const;  // count()*elemSize() contiguous bytes

    // DUPSORT escalation accessor -- only valid when isSubTreeAt(i).
    bool isSubTreeAt(std::uint16_t i) const;
    PageId subTreeRootAt(std::uint16_t i) const;

    // Value-level compression accessors -- only valid when isCompressedAt(i)
    // (which itself is only ever true when isOverflowAt(i) or the cell is
    // plain Inline).
    bool isCompressedAt(std::uint16_t i) const;
    std::uint64_t uncompressedLenAt(std::uint16_t i) const;

    // Index of the first entry whose key is >= `key` (standard lower_bound).
    // Returns count() if all entries are < key.
    std::uint16_t lowerBound(ByteView key) const { return lowerBound(key, KeyOrder{}); }
    std::uint16_t lowerBound(ByteView key, const KeyOrder& order) const;
    std::uint16_t lowerBoundKeyValue(ByteView key, ByteView value) const {
        return lowerBoundKeyValue(key, value, KeyOrder{});
    }
    std::uint16_t lowerBoundKeyValue(ByteView key, ByteView value, const KeyOrder& order) const;
    // Count of consecutive entries with exact `key` starting at lowerBound(key).
    std::uint16_t countDups(ByteView key) const;

    LeafEntry entryAt(std::uint16_t i) const;
    std::vector<LeafEntry> decodeAll() const;

private:
    struct Slot { std::uint16_t offset; std::uint16_t length; };
    Slot slotAt(std::uint16_t i) const;
    const std::byte* payload() const {
        return reinterpret_cast<const std::byte*>(header_) + sizeof(PageHeader);
    }
    const PageHeader* header_;
};

// Computes the encoded size (slot array + cells) of `entries`. Callers use
// this to decide whether a split is needed before attempting encodeLeaf.
std::size_t encodedLeafSize(const std::vector<LeafEntry>& entries);

// Encodes `entries` (must already be sorted by key, and by value too when
// dupSort composite ordering is in effect) into `header`'s payload. Returns
// false if they do not fit in `capacityBytes`; on success sets
// header->itemCount and header->payloadSize (caller still owns crc via
// PageManager::finalizePage).
bool encodeLeaf(PageHeader* header, std::uint32_t capacityBytes, const std::vector<LeafEntry>& entries);

// ---------------------------------------------------------------------------
// Branch entries
// ---------------------------------------------------------------------------

struct BranchEntry {
    ByteBuffer key;  // entry 0's key is always empty ("-infinity")
    PageId child = kInvalidPageId;
};

class BranchPageView {
public:
    explicit BranchPageView(const PageHeader* header) : header_(header) {}

    std::uint16_t count() const { return header_->itemCount; }
    ByteView keyAt(std::uint16_t i) const;
    PageId childAt(std::uint16_t i) const;

    // Index of the entry whose child subtree should contain `key`: the
    // largest i such that keyAt(i) <= key (entry 0's key is always
    // considered <= everything).
    std::uint16_t findChildIndex(ByteView key) const { return findChildIndex(key, KeyOrder{}); }
    std::uint16_t findChildIndex(ByteView key, const KeyOrder& order) const;

    BranchEntry entryAt(std::uint16_t i) const;
    std::vector<BranchEntry> decodeAll() const;

private:
    struct Slot { std::uint16_t offset; std::uint16_t length; };
    Slot slotAt(std::uint16_t i) const;
    const std::byte* payload() const {
        return reinterpret_cast<const std::byte*>(header_) + sizeof(PageHeader);
    }
    const PageHeader* header_;
};

std::size_t encodedBranchSize(const std::vector<BranchEntry>& entries);
bool encodeBranch(PageHeader* header, std::uint32_t capacityBytes, const std::vector<BranchEntry>& entries);

// Bounds-check every slot and cell on a Leaf or Branch page.
// Returns Corruption on any out-of-range offset, length, keyLen, or valueLen.
// Intended for ANNPOCKETDB_PARANOID builds; not called on the default path.
Status validateNodePage(const PageHeader* header, std::uint32_t pageSize);

}  // namespace annpocketdb::btree
