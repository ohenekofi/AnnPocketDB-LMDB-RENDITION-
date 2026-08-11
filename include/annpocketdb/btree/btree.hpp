#pragma once
//
// AnnPocketDb :: btree/btree.hpp
//
// The B+tree: sorted key storage, efficient range scans (via Cursor),
// copy-on-write mutation, and large-value overflow chains. This is the
// component the rest of the engine (named databases, secondary indexes,
// document storage) is built on top of -- the "heart" of the storage
// kernel per the architecture spec.
//
// Concurrency / MVCC contract: every write (insert/remove) copies every
// page along the root-to-leaf path into brand-new pages (never mutating an
// existing, possibly-reader-visible page in place) and returns the *new*
// root page id. The caller (WriteTransaction) is responsible for retiring
// the old root chain's now-superseded page ids via PageManager::retirePage
// as it becomes aware of them, and for committing the new root through
// PageManager::commit so it becomes visible to future readers atomically.
// A ReadTransaction simply keeps using the old root id it snapshotted at
// begin() -- since this class never overwrites a page an existing root
// might reference, that snapshot stays perfectly valid until the reader
// unregisters.
//
// Scope note: duplicate-key (DUPSORT) support is intentionally out of
// scope for this milestone (see README). Fill-factor rebalancing *is*
// implemented: deletion merges an underflowing node with a sibling when the
// combined content fits in one page, or redistributes (borrows) entries
// evenly between them otherwise, cascading up through every ancestor level
// exactly like a classic B-tree delete -- not just a single-level patch.
// Only the root is allowed to sit below the minimum fill threshold, since it
// has no sibling to rebalance against.
//
#include <optional>
#include "annpocketdb/btree/node_codec.hpp"
#include "annpocketdb/btree/db_flags.hpp"
#include "annpocketdb/core/page_manager.hpp"
#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

namespace annpocketdb::btree {

class BTree {
public:
    explicit BTree(core::PageManager& pageManager) : pm_(pageManager) {}

    // ---- Read path ----
    // Walk the tree and fill page/entry counters (mdb_stat).
    struct TreeStat {
        std::uint32_t depth = 0;
        std::uint64_t branchPages = 0;
        std::uint64_t leafPages = 0;
        std::uint64_t overflowPages = 0;
        std::uint64_t entries = 0;
    };
    Result<TreeStat> collectStat(PageId rootPageId) const;

    // `customOrder`, when non-null, is used verbatim INSTEAD of deriving a
    // KeyOrder from `flags` -- this is how a per-named-database custom
    // comparator (LMDB's mdb_set_compare/mdb_set_dupsort, see
    // api::SubDatabaseManager::setKeyComparator) actually takes effect at
    // the storage level, not just for a Cursor's own read-side navigation.
    Result<ByteBuffer> get(PageId rootPageId, ByteView key, DbFlags flags = DbFlags::None,
                          const KeyOrder* customOrder = nullptr) const;
    // Zero-copy get for inline values. Fails with InvalidArgument if the value
    // lives in an overflow chain (caller should use get() instead).
    Result<ByteView> getView(PageId rootPageId, ByteView key, DbFlags flags = DbFlags::None,
                             const KeyOrder* customOrder = nullptr) const;
    Result<bool> contains(PageId rootPageId, ByteView key, DbFlags flags = DbFlags::None,
                          const KeyOrder* customOrder = nullptr) const;
    // All values for `key` in key order (DUPSORT). Empty vector → NotFound.
    Result<std::vector<ByteBuffer>> getAll(PageId rootPageId, ByteView key, DbFlags flags = DbFlags::None,
                                           const KeyOrder* customOrder = nullptr) const;

    // ---- Write path: returns the new root page id ----
    // overwrite: replace the first matching key (unique DB semantics).
    // allowDups (DUPSORT): insert a new (key, value) pair sorted by (key, value);
    //   exact key+value match is a no-op when overwrite is false (NoDupData),
    //   or replaces that one entry when overwrite is true. Values larger than
    //   overflowThreshold() are rejected under allowDups (dups must be inline
    //   so leaf order can compare without materializing overflow chains).
    Result<PageId> insert(PageId rootPageId, TxnId txnId, ByteView key, ByteView value,
                           bool overwrite, bool allowDups = false, DbFlags flags = DbFlags::None,
                           const KeyOrder* customOrder = nullptr);
    // Fast path for pre-sorted bulk loads (LMDB's MDB_APPEND / Cursor::put's
    // Append flag): caller guarantees `key` sorts strictly after every key
    // already in the tree. Skips comparison-based descent entirely (goes
    // straight to the rightmost child at every branch level, and to the end
    // of the entry list at the leaf) instead of insertRecursive's normal
    // findChildIndex/lowerBound search. Defensively re-checks the ordering
    // guarantee against the actual rightmost leaf and returns
    // InvalidArgument (never corrupts the tree) if it's violated.
    Result<PageId> insertAppend(PageId rootPageId, TxnId txnId, ByteView key, ByteView value,
                                DbFlags flags = DbFlags::None);
    // Bulk dup-load fast path (LMDB's MDB_MULTIPLE, typically combined with
    // MDB_APPENDDUP): stores `valuesBlob` (a contiguous array of
    // `elemSize`-byte elements, caller-guaranteed sorted) as one
    // fixed-stride run under `key`. If `key` already holds a run with the
    // same elemSize, the new values are appended to the end of it (repeated
    // calls build up one key's run incrementally); mixing with ordinary
    // (non-DUPFIXED) dup storage for the same key is rejected with
    // InvalidArgument, never silently misinterpreted.
    Result<PageId> insertMultiple(PageId rootPageId, TxnId txnId, ByteView key, std::uint32_t elemSize,
                                  ByteView valuesBlob, DbFlags flags = DbFlags::None);
    // Delete every entry with `key` (all dups).
    Result<PageId> remove(PageId rootPageId, TxnId txnId, ByteView key, DbFlags flags = DbFlags::None,
                          const KeyOrder* customOrder = nullptr);
    // Delete the single (key, value) pair under DUPSORT.
    Result<PageId> removeOne(PageId rootPageId, TxnId txnId, ByteView key, ByteView value,
                             DbFlags flags = DbFlags::None, const KeyOrder* customOrder = nullptr);

    // Materializes a (possibly overflow-chained) value into an owned buffer.
    Result<ByteBuffer> materializeValue(const LeafEntry& entry) const;

    // In-order walk of a DUPSORT-escalation sub-tree (see node_codec.hpp's
    // SubTree cell kind), collecting every leaf key (the sub-tree's "keys"
    // ARE the original dup values -- its own "values" are unused markers).
    // Used by get()/getAll() (a SubTree-holding key's dup values) and by
    // Cursor's dup-navigation (materialized once per cursor landing, same
    // pattern as its DUPFIXED runIndex handling).
    Result<std::vector<ByteBuffer>> collectSubTreeValues(PageId subTreeRoot) const;
    // Same walk as collectSubTreeValues but counting only -- avoids
    // materializing every value just to answer Cursor::count().
    Result<std::size_t> countSubTreeValues(PageId subTreeRoot) const;

    // Exposed so Cursor can walk branch/leaf pages without re-implementing
    // the codec dispatch; also used by node-count/fill-factor diagnostics.
    core::PageManager& pageManager() { return pm_; }
    const core::PageManager& pageManager() const { return pm_; }

    // Keys are always inlined in leaf/branch cells. Capped at capacity/4 so
    // a single entry cannot monopolize a page (and so splits always have
    // room for at least two entries). Rejected at insert() with
    // InvalidArgument. Exposed publicly for Environment::maxKeySize().
    std::size_t maxKeySize() const { return core::payloadCapacity(pm_.pageSize()) / 4; }

private:
    struct InsertOutcome {
        PageId leftPageId = kInvalidPageId;
        std::optional<ByteBuffer> splitKey;
        PageId rightPageId = kInvalidPageId;
    };
    Result<InsertOutcome> insertRecursive(PageId nodePageId, TxnId txnId, ByteView key, ByteView value,
                                           bool overwrite, bool allowDups, KeyOrder order,
                                           DbFlags flags, std::uint32_t depth = 0,
                                           bool appendMode = false);
    // Separate recursion for insertMultiple: a FixedRun cell is a distinct
    // physical entry shape from insertRecursive's per-value dup cells, kept
    // intentionally isolated rather than woven into the heavily-shared
    // insertRecursive to avoid any interaction risk with the allowDups/
    // appendMode/overwrite paths every other write already depends on.
    Result<InsertOutcome> insertMultipleRecursive(PageId nodePageId, TxnId txnId, ByteView key,
                                                   std::uint32_t elemSize, ByteView valuesBlob,
                                                   KeyOrder order, std::uint32_t depth = 0);
    // Shared root-split epilogue for insert()/insertAppend(): wraps a split
    // InsertOutcome in a brand-new two-child branch root, or just returns
    // the unsplit root as-is.
    Result<PageId> finishInsertAtRoot(TxnId txnId, const InsertOutcome& outcome);

    // A node's removal result is one of three shapes:
    //  - Empty:      the node has zero entries left; the parent must drop
    //                the corresponding branch entry entirely.
    //  - Underflow:  the node has entries but its encoded size fell below
    //                the minimum fill threshold; it is deliberately left
    //                *unpersisted* (no page allocated yet) so the parent can
    //                merge or redistribute it with a sibling before either
    //                one is ever written to disk.
    //  - Ok:         the node was persisted as-is (adequately filled, or a
    //                collapsed single-child branch reusing its child's
    //                already-persisted page).
    enum class RemoveKind { Empty, Underflow, Ok };
    struct RemoveOutcome {
        RemoveKind kind = RemoveKind::Ok;
        core::PageType nodeType = core::PageType::Leaf;  // Leaf or Branch
        PageId pageId = kInvalidPageId;                  // valid iff kind == Ok
        std::vector<LeafEntry> leafEntries;               // valid iff Underflow && nodeType == Leaf
        std::vector<BranchEntry> branchEntries;           // valid iff Underflow && nodeType == Branch
    };
    // matchValue: if non-null, only remove the (key, *matchValue) pair; else all dups of key.
    Result<RemoveOutcome> removeRecursive(PageId nodePageId, TxnId txnId, ByteView key,
                                           const ByteView* matchValue, KeyOrder order,
                                           std::uint32_t depth = 0);

    // Rebalances the child at `childIdx` (currently Underflow, contents in
    // `childOutcome`) against one of its siblings, mutating `entries` in
    // place to reflect the merge or redistribution. Used from the Branch
    // case of removeRecursive for both leaf-level and branch-level children.
    Status rebalanceLeafChild(std::vector<BranchEntry>& entries, std::size_t childIdx, TxnId txnId,
                               std::vector<LeafEntry>&& childEntries);
    Status rebalanceBranchChild(std::vector<BranchEntry>& entries, std::size_t childIdx, TxnId txnId,
                                 std::vector<BranchEntry>&& childEntries);

    Result<PageId> writeOverflowChain(TxnId txnId, ByteView value);
    Result<ByteBuffer> readOverflowChain(PageId firstPage, std::uint64_t totalLen) const;
    Status retireOverflowChain(PageId firstPage, TxnId txnId);

    // Value-level compression (see compression/lz4_codec.hpp). Only ever
    // called on plain, non-dup, non-fixed-stride values -- DUPSORT dup
    // values must stay byte-comparable for seekRangeDup/cursor range order,
    // and DUPFIXED runs require uniform per-element stride; compression
    // would break both, so callers never invoke this for those paths.
    // Returns `value` unchanged (entry.isCompressed left false) if
    // compression is disabled, the value is below the configured minimum
    // size, or compressing it wouldn't shrink it. On success, sets
    // entry.isCompressed/uncompressedLen and returns a view over `scratch`
    // (caller-owned; must outlive the returned view), which holds the
    // compressed bytes.
    ByteView maybeCompress(ByteView value, ByteBuffer& scratch, LeafEntry& entry) const;

    // Checks whether the dup-group [groupStart, groupEnd) in `entries` (all
    // sharing one key, DUPSORT storage) is a good candidate for sub-tree
    // escalation, and if so, replaces it in place with one SubTree LeafEntry.
    // Only escalates when the group does NOT touch either edge of `entries`
    // (groupStart > 0 && groupEnd < entries.size()) -- this is what makes it
    // safe without also having to guarantee dup-groups never straddle a leaf
    // split: if the group's only partially visible in this leaf, we simply
    // don't escalate yet (it naturally becomes eligible once the leaf/split
    // layout consolidates it), rather than escalating a partial group and
    // risking leaving more flat dups for the same key elsewhere.
    Status escalateDupGroupIfNeeded(std::vector<LeafEntry>& entries, TxnId txnId,
                                    const KeyOrder& order, std::uint32_t capacity);
    // Retires every page (branch and leaf -- a sub-tree's own values are
    // always empty markers, so it never has overflow chains) reachable from
    // a DUPSORT escalation sub-tree's root. Called when the flat cell that
    // used to hold this key's whole dup-group is removed (BTree::remove).
    Status retireSubTree(PageId subTreeRoot, TxnId txnId);

    std::size_t overflowThreshold() const { return core::payloadCapacity(pm_.pageSize()) / 4; }
    // Nodes below this encoded size (bytes) are considered underflowing and
    // eligible for merge/redistribution with a sibling. A quarter of page
    // capacity is a standard target minimum fill factor.
    std::size_t minFillThreshold() const { return core::payloadCapacity(pm_.pageSize()) / 4; }

    core::PageManager& pm_;
};

}  // namespace annpocketdb::btree
