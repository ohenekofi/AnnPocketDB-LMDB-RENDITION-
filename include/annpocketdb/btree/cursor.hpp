#pragma once
//
// AnnPocketDb :: btree/cursor.hpp
//
// Ordered iteration over a BTree snapshot. Leaf pages do not carry sibling
// pointers (COW stays path-local); the cursor keeps a parent stack and
// re-descends when walking off a leaf edge.
//
// Write-capable cursors (opened from WriteTransaction) may also:
//   - delete the current record
//   - put relative to the current position (CURRENT / after-key insert)
//   - navigate duplicate groups under DUPSORT
//
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "annpocketdb/btree/node_codec.hpp"
#include "annpocketdb/btree/db_flags.hpp"
#include "annpocketdb/core/page_manager.hpp"
#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

namespace annpocketdb::btree {

class BTree;

using RootAccessor = std::function<PageId()>;

// Flags for Cursor::put (LMDB-inspired).
enum class CursorPutFlags : std::uint32_t {
    // Insert / overwrite by key (unique) or by (key,value) when AppendDup.
    None = 0,
    // Fail if the key (or exact pair under AppendDup) already exists.
    NoOverwrite = 1u << 0,
    // DUPSORT insert: keep other values for the same key.
    AppendDup = 1u << 1,
    // Replace the value of the *current* entry; key must match current key.
    // Equivalent to LMDB MDB_CURRENT.
    Current = 1u << 2,
    // Caller guarantees `key` sorts strictly after every key already in the
    // tree (LMDB MDB_APPEND): skips comparison-based descent (goes straight
    // to the rightmost child/leaf at every level) and the leaf-level binary
    // search for insertion position. Returns InvalidArgument instead of
    // corrupting the tree if the order guarantee is violated.
    Append = 1u << 3,
};

inline CursorPutFlags operator|(CursorPutFlags a, CursorPutFlags b) {
    return static_cast<CursorPutFlags>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
inline bool hasFlag(CursorPutFlags set, CursorPutFlags f) {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(f)) != 0;
}

class Cursor {
public:
    Cursor(const BTree& tree, RootAccessor rootAccessor)
        : tree_(tree), rootAccessor_(std::move(rootAccessor)) {}

    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;
    Cursor(Cursor&&) noexcept = default;
    Cursor& operator=(Cursor&&) noexcept = default;

    // Enable delete/put. `root` is updated after each mutating call.
    // Only valid while the owning WriteTransaction is alive.
    //
    // `generationAccessor`, when supplied, lets the cursor detect that its
    // owning WriteTransaction was moved/destroyed OR that a nested
    // transaction it was opened during has since been aborted (both bump
    // WriteTransaction::generation_) -- either way, this cursor's stack_ may
    // reference pages a rolled-back COW root no longer reaches. Every
    // positioning/mutating call re-checks the captured value and fails
    // instead of silently returning stale data.
    void bindWrite(BTree& tree, PageId& root, TxnId txnId,
                    std::function<std::uint64_t()> generationAccessor = {}) {
        writeTree_ = &tree;
        writeRoot_ = &root;
        writeTxnId_ = txnId;
        writeGenerationAccessor_ = std::move(generationAccessor);
        capturedGeneration_ = writeGenerationAccessor_ ? writeGenerationAccessor_() : 0;
    }
    void setFlags(DbFlags flags) {
        flags_ = effectiveFlags(flags);
        order_ = KeyOrder::fromFlags(flags_);
    }
    DbFlags flags() const { return flags_; }
    // Layers a custom comparator (LMDB's mdb_set_compare/mdb_set_dupsort) on
    // top of whatever setFlags() already derived -- call AFTER setFlags(),
    // since setFlags() replaces order_ wholesale. Once set, this cursor's
    // OWN navigation (seek/lowerBound/etc, already comparator-driven via
    // order_) AND its write path (put()/del(), which now pass &order_ to
    // BTree instead of just flags_) both honor it -- not just reads.
    void setCustomComparators(std::function<int(ByteView, ByteView)> keyCmp,
                              std::function<int(ByteView, ByteView)> valueCmp) {
        order_.customKeyCmp = std::move(keyCmp);
        order_.customValueCmp = std::move(valueCmp);
    }
    bool isWriteBound() const { return writeTree_ != nullptr && writeRoot_ != nullptr; }

    // ---- Positioning ----
    Result<bool> first();
    Result<bool> last();
    Result<bool> seek(ByteView key);       // first >= key (MDB_SET_RANGE)
    Result<bool> seekExact(ByteView key);  // first exact key (MDB_SET)
    // DUPSORT: position at exact (key, value). MDB_GET_BOTH.
    Result<bool> seekKeyValue(ByteView key, ByteView value);

    Result<bool> next();
    Result<bool> prev();

    // ---- Duplicate-group navigation (DUPSORT) ----
    // All require the cursor to already be on a key (or seekExact first).
    Result<bool> firstDup();   // first value of the current key (MDB_FIRST_DUP)
    Result<bool> lastDup();    // last value of the current key (MDB_LAST_DUP)
    Result<bool> nextDup();    // next value with the same key (MDB_NEXT_DUP)
    Result<bool> prevDup();    // previous value with the same key (MDB_PREV_DUP)
    // Skip to first entry of the next key (MDB_NEXT_NODUP).
    Result<bool> nextNoDup();
    // Skip to last entry of the previous key (MDB_PREV_NODUP).
    Result<bool> prevNoDup();

    // ---- Access ----
    bool valid() const { return !stack_.empty(); }
    Result<ByteView> key() const;
    Result<ByteView> valueView() const;
    Result<ByteBuffer> value() const;
    bool isOverflowValue() const;

    // ---- Mutation (write-bound cursors only) ----
    // Delete the current entry (MDB_cursor_del). Positions on the next entry
    // if any; otherwise the cursor becomes invalid.
    Status del();
    // Put with LMDB-style flags. On success the cursor is positioned on the
    // written entry. For Current, `key` must equal the current key (may be empty
    // to mean "use current key").
    Status put(ByteView key, ByteView value, CursorPutFlags flags = CursorPutFlags::None);

    // Bulk dup-load (LMDB's MDB_MULTIPLE): stores `valuesBlob` (a contiguous
    // array of `elemSize`-byte elements, caller-guaranteed sorted) as one
    // fixed-stride run under `key`, appending to an existing run for the
    // same key if present. See BTree::insertMultiple for the full contract.
    Status putMultiple(ByteView key, std::uint32_t elemSize, ByteView valuesBlob);

    // ---- Duplicate-count / batch convenience (LMDB Cursor.count/getmulti/putmulti/replace/pop) ----

    // Number of values ("duplicates") for the current key; 1 for a
    // non-DUPSORT key. Caveat: an unescalated (flat) dup-group is not
    // guaranteed never to straddle a leaf boundary (see
    // BTree::escalateDupGroupIfNeeded's header comment) -- in that rare,
    // already-disclosed case this undercounts, exactly like the rest of
    // this codebase's flat dup-storage handling.
    Result<std::size_t> count() const;

    // Seeks to `key`, replaces its (first/unique) value with `value`, and
    // returns the value that was there before (nullopt if the key didn't
    // exist -- the put still happens either way). Scoped to the non-dup /
    // "current value of the key" case, matching LMDB's Cursor.replace().
    Result<std::optional<ByteBuffer>> replace(ByteView key, ByteView value);
    // Seeks to `key`, removes it, and returns the value that was there
    // (nullopt if the key didn't exist). LMDB's Cursor.pop().
    Result<std::optional<ByteBuffer>> pop(ByteView key);

    // Looks up each of `keys` independently (repositioning the cursor each
    // time), silently skipping any that don't exist -- matches LMDB's
    // Cursor.getmulti(). When `dupData` is true on a DUPSORT database,
    // every value for a matching key is included (one (key,value) pair per
    // dup), not just the first.
    Result<std::vector<std::pair<ByteBuffer, ByteBuffer>>> getMulti(const std::vector<ByteView>& keys,
                                                                     bool dupData = false);

    struct PutMultiResult {
        std::size_t consumed = 0;  // items processed (== items.size() unless a real error aborted early)
        std::size_t added = 0;     // items actually written (excludes overwrite=false collisions)
    };
    // Writes every (key, value) pair in `items`, stopping and returning
    // early only on a genuine error -- an overwrite=false collision is not
    // fatal, it's just excluded from `added` (matches LMDB's
    // Cursor.putmulti() (consumed, added) return). `append` asserts the
    // pairs are already in ascending key order and uses the Append fast
    // path for every one of them (LMDB's putmulti(append=True)); exactly
    // like a single Append put, a violated ordering guarantee aborts the
    // whole call with InvalidArgument rather than corrupting the tree.
    Result<PutMultiResult> putMulti(const std::vector<std::pair<ByteView, ByteView>>& items,
                                    bool overwrite = true, bool append = false);

    // DUPSORT: seeks to `key`, positioned at the first value >= `value`
    // (LMDB's MDB_GET_BOTH_RANGE / Cursor.set_range_dup()).
    Result<bool> seekRangeDup(ByteView key, ByteView value);

private:
    struct Frame {
        PageId pageId;
        std::uint16_t index;
        std::uint16_t count;
        // Sub-position within a DUPFIXED fixed-run cell OR a DUPSORT
        // escalation sub-tree at `index` (0 when `index` is neither -- an
        // ordinary Inline/Overflow cell -- or at the group's first element).
        // Only ever meaningful on the top (leaf) frame. Mutually exclusive
        // with subTreeValues below (a cell is one kind or the other).
        std::uint32_t runIndex = 0;
        // Materialized dup values when `index` is a SubTree cell (see
        // node_codec.hpp), populated once per landing (descendAppendFirst/
        // Last/ToKey) via BTree::collectSubTreeValues -- shared_ptr so Frame
        // stays cheaply copyable/movable. nullptr when not on a SubTree cell.
        std::shared_ptr<std::vector<ByteBuffer>> subTreeValues;
    };

    // True when the cursor is positioned on a leaf cell storing a DUPFIXED
    // fixed-stride run (see node_codec.hpp's FixedRun cell kind).
    bool isCurrentFixedRun() const;
    // True when the cursor is positioned on a DUPSORT escalation sub-tree
    // cell (see node_codec.hpp's SubTree cell kind).
    bool isCurrentSubTree() const;
    // Materializes and caches (in the top frame) the current SubTree cell's
    // dup values; no-op if already cached or not on a SubTree cell. const
    // because it only populates a lazy cache (stack_ is mutable for exactly
    // this reason), called from valueView()/value()/next()/prev().
    Status ensureSubTreeValuesCached() const;

    Status descendAppendFirst(PageId pageId);
    Status descendAppendLast(PageId pageId);
    Status descendAppendToKey(PageId pageId, ByteView key);
    Result<bool> ascendAndAdvance();
    Result<bool> ascendAndRetreat();

    // Copy current key into owned buffer (needed across mutations).
    Result<ByteBuffer> currentKeyOwned() const;
    Result<ByteBuffer> currentValueOwned() const;

    // OK unless write-bound and the owning transaction's generation moved on
    // from what was captured at bindWrite() time (move/destroy or a nested
    // abort since this cursor was opened).
    Status checkGeneration() const;

    const BTree& tree_;
    const core::PageManager& pageManager() const;

    RootAccessor rootAccessor_;
    // mutable so valueView()/value() (const accessors) can lazily populate a
    // SubTree cell's materialized dup values on first access, matching this
    // codebase's existing lazy-cache-under-const convention (see
    // txn::ReadTransaction::subDbs_).
    mutable std::vector<Frame> stack_;

    BTree* writeTree_ = nullptr;
    PageId* writeRoot_ = nullptr;
    TxnId writeTxnId_ = 0;
    DbFlags flags_ = DbFlags::None;
    KeyOrder order_{};

    std::function<std::uint64_t()> writeGenerationAccessor_;
    std::uint64_t capturedGeneration_ = 0;
};

}  // namespace annpocketdb::btree
