#include "annpocketdb/btree/btree.hpp"
#include "annpocketdb/util/bounded_walk.hpp"
#include "annpocketdb/compression/lz4_codec.hpp"
#include <cstring>
#include <algorithm>
#include <vector>
namespace annpocketdb::btree {
using core::PageHeader;
using core::PageType;

namespace {
// Defined further down this file; forward-declared here because they're
// used by split logic earlier in the file. Multiple unnamed-namespace
// blocks in one translation unit refer to the same anonymous namespace, so
// this and the later definitions are the same entities.
std::size_t pickLeafSplitIndex(const std::vector<LeafEntry>& entries, std::uint32_t capacity);
std::size_t pickBranchSplitIndex(const std::vector<BranchEntry>& entries, std::uint32_t capacity);
}  // namespace
// ---------------------------------------------------------------------------
// Value-level compression
// ---------------------------------------------------------------------------
ByteView BTree::maybeCompress(ByteView value, ByteBuffer& scratch, LeafEntry& entry) const {
#if defined(ANNPOCKETDB_WITH_COMPRESSION)
    if (pm_.compressionEnabled() && value.size() >= pm_.compressionMinSize()) {
        if (compression::compress(value, scratch)) {
            entry.isCompressed = true;
            entry.uncompressedLen = value.size();
            return toByteView(scratch);
        }
    }
#else
    (void)scratch;
    (void)entry;
#endif
    return value;
}

// ---------------------------------------------------------------------------
// Overflow chains for large values
// ---------------------------------------------------------------------------
Result<PageId> BTree::writeOverflowChain(TxnId txnId, ByteView value) {
    std::size_t capacity = core::payloadCapacity(pm_.pageSize());
    std::size_t total = value.size();
    std::size_t numChunks = (total + capacity - 1) / capacity;
    if (numChunks == 0) numChunks = 1;
    PageId nextLink = kInvalidPageId;
    for (std::size_t i = numChunks; i-- > 0;) {
        auto allocResult = pm_.allocatePage(txnId, PageType::Overflow);
        if (!allocResult) return allocResult.status();
        PageHeader* header = allocResult.value();
        std::size_t offset = i * capacity;
        std::size_t len = std::min(capacity, total - offset);
        std::memcpy(core::PageManager::payload(header), value.data() + offset, len);
        header->overflowNext = nextLink;
        pm_.finalizePage(header, static_cast<std::uint32_t>(len), 0);
        nextLink = header->pageId;
    }
    return Result<PageId>(nextLink);
}
Result<ByteBuffer> BTree::readOverflowChain(PageId firstPage, std::uint64_t totalLen) const {
    // Same class of bug as BlobReader::loadHead: a corrupted leaf can claim an
    // enormous totalLen and force an OOM on the materializing allocation.
    // A value cannot exceed the live file contents (pageCount * payload), and
    // must also stay within the mapped reservation.
    const std::uint64_t pageBytes =
        static_cast<std::uint64_t>(std::max<std::uint64_t>(pm_.pageCount(), 1)) *
        core::payloadCapacity(pm_.pageSize());
    const std::uint64_t maxBytes = std::min(pageBytes, pm_.mappedFile().mappedSize());
    if (totalLen > maxBytes) {
        return Status::Corruption(
            "overflow totalLen " + std::to_string(totalLen) +
            " exceeds maximum " + std::to_string(maxBytes));
    }
    ByteBuffer result(static_cast<std::size_t>(totalLen));
    PageId cur = firstPage;
    std::uint64_t written = 0;
    BoundedWalk walk(std::max<std::uint64_t>(pm_.pageCount(), 1) + 2);
    while (cur != kInvalidPageId) {
        Status bs = walk.step("overflow read chain");
        if (!bs.ok()) return bs;
        auto pageResult = pm_.getPage(cur);
        if (!pageResult) return pageResult.status();
        const PageHeader* header = pageResult.value();
        if (header->type != PageType::Overflow) {
            return Status::Corruption("overflow chain page has unexpected type");
        }
        std::uint64_t len = header->payloadSize;
        if (written + len > totalLen) len = totalLen - written;
        std::memcpy(result.data() + written, core::PageManager::payload(header),
                    static_cast<std::size_t>(len));
        written += len;
        cur = header->overflowNext;
    }
    if (written != totalLen) return Status::Corruption("overflow chain length mismatch");
    return Result<ByteBuffer>(std::move(result));
}
Status BTree::retireOverflowChain(PageId firstPage, TxnId txnId) {
    PageId cur = firstPage;
    BoundedWalk walk(std::max<std::uint64_t>(pm_.pageCount(), 1) + 2);
    while (cur != kInvalidPageId) {
        Status bs = walk.step("overflow retire chain");
        if (!bs.ok()) return bs;
        auto pageResult = pm_.getPage(cur);
        if (!pageResult) return pageResult.status();
        PageId next = pageResult.value()->overflowNext;
        pm_.retirePage(cur, txnId);
        cur = next;
    }
    return Status::OK();
}
Result<ByteBuffer> BTree::materializeValue(const LeafEntry& entry) const {
    Result<ByteBuffer> stored = entry.isOverflow
        ? readOverflowChain(entry.overflowFirstPage, entry.overflowTotalLen)
        : Result<ByteBuffer>(entry.value);
    if (!stored) return stored;
    if (!entry.isCompressed) return stored;
#if defined(ANNPOCKETDB_WITH_COMPRESSION)
    // Same class of defense as readOverflowChain's totalLen bound: a
    // corrupted uncompressedLen must not be allowed to force an unbounded
    // allocation before decompression even runs. Unlike readOverflowChain's
    // bound, this can NOT be checked against current physical page count --
    // compression's entire point is that the logical (uncompressed) length
    // can far exceed the physical bytes actually stored. The database's
    // virtual reservation ceiling (mappedSize(), e.g. 1 TiB by default) is
    // the true universal upper bound any value could ever legitimately have.
    const std::uint64_t maxBytes = pm_.mappedFile().mappedSize();
    if (entry.uncompressedLen > maxBytes) {
        return Status::Corruption(
            "compressed value's declared uncompressedLen " + std::to_string(entry.uncompressedLen) +
            " exceeds maximum " + std::to_string(maxBytes));
    }
    ByteBuffer decompressed;
    Status ds = compression::decompress(toByteView(stored.value()), entry.uncompressedLen, decompressed);
    if (!ds.ok()) return ds;
    return Result<ByteBuffer>(std::move(decompressed));
#else
    return Status::NotSupported(
        "value is compressed; this build lacks compression support (ANNPOCKETDB_WITH_COMPRESSION)");
#endif
}

namespace {
Status collectSubTreeValuesRecursive(const core::PageManager& pm, PageId pageId,
                                      std::vector<ByteBuffer>& out, std::uint32_t depth) {
    if (depth > kMaxTreeDepth) return Status::Corruption("sub-tree walk depth exceeded bound");
    auto pr = pm.getPage(pageId);
    if (!pr) return pr.status();
    const PageHeader* h = pr.value();
    if (h->type == PageType::Leaf) {
        LeafPageView v(h);
        for (std::uint16_t i = 0; i < v.count(); ++i) {
            out.push_back(toByteBuffer(v.keyAt(i)));
        }
        return Status::OK();
    }
    if (h->type == PageType::Branch) {
        BranchPageView v(h);
        for (std::uint16_t i = 0; i < v.count(); ++i) {
            Status cs = collectSubTreeValuesRecursive(pm, v.childAt(i), out, depth + 1);
            if (!cs.ok()) return cs;
        }
        return Status::OK();
    }
    return Status::Corruption("unexpected page type in sub-tree walk");
}
}  // namespace

Result<std::vector<ByteBuffer>> BTree::collectSubTreeValues(PageId subTreeRoot) const {
    std::vector<ByteBuffer> out;
    if (subTreeRoot == kInvalidPageId) return Result<std::vector<ByteBuffer>>(std::move(out));
    Status s = collectSubTreeValuesRecursive(pm_, subTreeRoot, out, 0);
    if (!s.ok()) return s;
    return Result<std::vector<ByteBuffer>>(std::move(out));
}

namespace {
Result<std::size_t> countSubTreeValuesRecursive(const core::PageManager& pm, PageId pageId, std::uint32_t depth) {
    if (depth > kMaxTreeDepth) return Status::Corruption("sub-tree walk depth exceeded bound");
    auto pr = pm.getPage(pageId);
    if (!pr) return pr.status();
    const PageHeader* h = pr.value();
    if (h->type == PageType::Leaf) {
        LeafPageView v(h);
        return Result<std::size_t>(static_cast<std::size_t>(v.count()));
    }
    if (h->type == PageType::Branch) {
        BranchPageView v(h);
        std::size_t total = 0;
        for (std::uint16_t i = 0; i < v.count(); ++i) {
            auto c = countSubTreeValuesRecursive(pm, v.childAt(i), depth + 1);
            if (!c) return c.status();
            total += c.value();
        }
        return Result<std::size_t>(total);
    }
    return Status::Corruption("unexpected page type in sub-tree walk");
}
}  // namespace

Result<std::size_t> BTree::countSubTreeValues(PageId subTreeRoot) const {
    if (subTreeRoot == kInvalidPageId) return Result<std::size_t>(std::size_t{0});
    return countSubTreeValuesRecursive(pm_, subTreeRoot, 0);
}

namespace {
Status retireSubTreeRecursive(core::PageManager& pm, PageId pageId, TxnId txnId, std::uint32_t depth) {
    if (depth > kMaxTreeDepth) return Status::Corruption("sub-tree retire depth exceeded bound");
    auto pr = pm.getPage(pageId);
    if (!pr) return pr.status();
    const PageHeader* h = pr.value();
    if (h->type == PageType::Branch) {
        BranchPageView v(h);
        for (std::uint16_t i = 0; i < v.count(); ++i) {
            Status s = retireSubTreeRecursive(pm, v.childAt(i), txnId, depth + 1);
            if (!s.ok()) return s;
        }
    } else if (h->type != PageType::Leaf) {
        return Status::Corruption("unexpected page type in sub-tree retire walk");
    }
    pm.retirePage(pageId, txnId);
    return Status::OK();
}
}  // namespace

Status BTree::retireSubTree(PageId subTreeRoot, TxnId txnId) {
    if (subTreeRoot == kInvalidPageId) return Status::OK();
    return retireSubTreeRecursive(pm_, subTreeRoot, txnId, 0);
}

// ---------------------------------------------------------------------------
// Read path
// ---------------------------------------------------------------------------

Result<BTree::TreeStat> BTree::collectStat(PageId rootPageId) const {
    TreeStat st;
    if (rootPageId == kInvalidPageId) return Result<TreeStat>(st);

    std::vector<std::pair<PageId, std::uint32_t>> stack;
    stack.emplace_back(rootPageId, 1);
    BoundedWalk pages(pm_.pageCount() + 64);

    while (!stack.empty()) {
        auto [pid, depth] = stack.back();
        stack.pop_back();
        Status ps = pages.step("collectStat page walk");
        if (!ps.ok()) return ps;

        auto pr = pm_.getPage(pid);
        if (!pr) return pr.status();
        const PageHeader* h = pr.value();
        if (depth > st.depth) st.depth = depth;

        if (h->type == PageType::Leaf) {
            ++st.leafPages;
            LeafPageView view(h);
            st.entries += view.count();
            for (std::uint16_t i = 0; i < view.count(); ++i) {
                if (!view.isOverflowAt(i)) continue;
                PageId cur = view.entryAt(i).overflowFirstPage;
                BoundedWalk chain(pm_.pageCount() + 8);
                while (cur != kInvalidPageId) {
                    Status cs = chain.step("collectStat overflow");
                    if (!cs.ok()) return cs;
                    auto op = pm_.getPage(cur);
                    if (!op) return op.status();
                    ++st.overflowPages;
                    cur = op.value()->overflowNext;
                }
            }
        } else if (h->type == PageType::Branch) {
            ++st.branchPages;
            BranchPageView view(h);
            for (std::uint16_t i = 0; i < view.count(); ++i) {
                stack.emplace_back(view.childAt(i), depth + 1);
            }
        } else if (h->type == PageType::Overflow) {
            ++st.overflowPages;
        }
        // ignore Meta/Free if encountered
    }
    return Result<TreeStat>(st);
}

Result<ByteBuffer> BTree::get(PageId rootPageId, ByteView key, DbFlags flags, const KeyOrder* customOrder) const {
    KeyOrder order = customOrder ? *customOrder : KeyOrder::fromFlags(flags);
    if (rootPageId == kInvalidPageId) return Status::NotFound();
    PageId cur = rootPageId;
    BoundedWalk depth(kMaxTreeDepth);
    while (true) {
        Status ds = depth.step("btree get depth");
        if (!ds.ok()) return ds;
        auto pageResult = pm_.getPage(cur);
        if (!pageResult) return pageResult.status();
        const PageHeader* header = pageResult.value();
        if (header->type == PageType::Leaf) {
            LeafPageView view(header);
            std::uint16_t idx = view.lowerBound(key, order);
            if (idx >= view.count() || order.cmpKey(view.keyAt(idx), key) != 0) return Status::NotFound();
            if (view.isSubTreeAt(idx)) {
                // DUPSORT convention: get() on a multi-valued key returns
                // the FIRST dup value (matching the flat-storage case's
                // "lowerBound lands on the first cell" behavior) -- the
                // sub-tree's own leftmost key is that first dup value.
                auto vals = collectSubTreeValues(view.subTreeRootAt(idx));
                if (!vals) return vals.status();
                if (vals.value().empty()) return Status::Corruption("empty DUPSORT sub-tree");
                return Result<ByteBuffer>(vals.value().front());
            }
            LeafEntry entry = view.entryAt(idx);
            return materializeValue(entry);
        } else if (header->type == PageType::Branch) {
            BranchPageView view(header);
            std::uint16_t idx = view.findChildIndex(key, order);
            cur = view.childAt(idx);
        } else {
            return Status::Corruption("unexpected page type while traversing btree");
        }
    }
}
Result<ByteView> BTree::getView(PageId rootPageId, ByteView key, DbFlags flags, const KeyOrder* customOrder) const {
    KeyOrder order = customOrder ? *customOrder : KeyOrder::fromFlags(flags);
    if (rootPageId == kInvalidPageId) return Status::NotFound();
    PageId cur = rootPageId;
    BoundedWalk depth(kMaxTreeDepth);
    while (true) {
        Status ds = depth.step("btree getView depth");
        if (!ds.ok()) return ds;
        auto pageResult = pm_.getPage(cur);
        if (!pageResult) return pageResult.status();
        const PageHeader* header = pageResult.value();
        if (header->type == PageType::Leaf) {
            LeafPageView view(header);
            std::uint16_t idx = view.lowerBound(key, order);
            if (idx >= view.count() || order.cmpKey(view.keyAt(idx), key) != 0) {
                return Status::NotFound();
            }
            if (view.isOverflowAt(idx)) {
                return Status::InvalidArgument(
                    "value is overflow-chained; use get() to materialize");
            }
            if (view.isFixedRunAt(idx)) {
                return Status::InvalidArgument(
                    "key holds a DUPFIXED run of values; use getAll() to materialize");
            }
            if (view.isSubTreeAt(idx)) {
                return Status::InvalidArgument(
                    "key holds a DUPSORT escalation sub-tree; use get()/getAll() to materialize");
            }
            if (view.isCompressedAt(idx)) {
                return Status::InvalidArgument(
                    "value is compressed; use get() to materialize");
            }
            return Result<ByteView>(view.inlineValueAt(idx));
        } else if (header->type == PageType::Branch) {
            BranchPageView view(header);
            std::uint16_t idx = view.findChildIndex(key, order);
            cur = view.childAt(idx);
        } else {
            return Status::Corruption("unexpected page type while traversing btree");
        }
    }
}

Result<bool> BTree::contains(PageId rootPageId, ByteView key, DbFlags flags, const KeyOrder* customOrder) const {
    auto r = get(rootPageId, key, flags, customOrder);
    if (r.ok()) return Result<bool>(true);
    if (r.status().code() == StatusCode::NotFound) return Result<bool>(false);
    return r.status();
}
// ---------------------------------------------------------------------------
// Write path: insert
// ---------------------------------------------------------------------------

Status BTree::escalateDupGroupIfNeeded(std::vector<LeafEntry>& entries, TxnId txnId,
                                        const KeyOrder& order, std::uint32_t capacity) {
    // Scan the whole (already page-capacity-bounded) entries vector for any
    // contiguous same-key run that (a) isn't already a single FixedRun/
    // SubTree cell, (b) has at least 2 members, (c) doesn't touch either
    // edge of `entries` (see the header comment: a group only partially
    // visible in this leaf is left alone rather than risking an escalation
    // that doesn't account for more dups of the same key elsewhere), and
    // (d) whose combined value bytes exceed half a page. This re-scans
    // after every ordinary dup insert rather than tracking "which group did
    // this call touch" -- simpler, and self-healing (catches any eligible
    // group, not just the one this particular call grew).
    std::size_t i = 0;
    while (i < entries.size()) {
        std::size_t j = i + 1;
        while (j < entries.size() &&
               order.cmpKey(toByteView(entries[j].key), toByteView(entries[i].key)) == 0) {
            ++j;
        }
        const bool touchesEdge = (i == 0) || (j == entries.size());
        const bool alreadyEscalated = entries[i].isSubTree || entries[i].fixedElemSize != 0;
        if (j - i >= 2 && !touchesEdge && !alreadyEscalated) {
            // Must reflect the group's actual encoded footprint (key repeated
            // per entry + per-cell/slot overhead), not just raw value bytes --
            // otherwise a leaf can hit its real capacity() limit and need an
            // ordinary split (which flat DUPSORT storage cannot safely span,
            // since branch descent is key-only) before this threshold ever
            // fires. encodedLeafSize on just the group gives the exact
            // bytes it would occupy on its own.
            std::vector<LeafEntry> group(entries.begin() + static_cast<long>(i),
                                          entries.begin() + static_cast<long>(j));
            std::size_t groupBytes = encodedLeafSize(group);
            if (groupBytes > static_cast<std::size_t>(capacity) / 2) {
                KeyOrder subOrder;
                subOrder.customKeyCmp = [order](ByteView a, ByteView b) { return order.cmpValue(a, b); };
                PageId subRoot = kInvalidPageId;
                for (std::size_t k = i; k < j; ++k) {
                    auto r = insert(subRoot, txnId, toByteView(entries[k].value), ByteView{},
                                    /*overwrite=*/true, /*allowDups=*/false, DbFlags::None, &subOrder);
                    if (!r) return r.status();
                    subRoot = r.value();
                }
                LeafEntry subEntry;
                subEntry.key = entries[i].key;
                subEntry.isSubTree = true;
                subEntry.subTreeRoot = subRoot;
                entries.erase(entries.begin() + static_cast<long>(i), entries.begin() + static_cast<long>(j));
                entries.insert(entries.begin() + static_cast<long>(i), subEntry);
                i += 1;  // entries shrank; continue scanning right after the new cell
                continue;
            }
        }
        i = j;
    }
    return Status::OK();
}

Result<BTree::InsertOutcome> BTree::insertRecursive(PageId nodePageId, TxnId txnId, ByteView key, ByteView value,
                                                     bool overwrite, bool allowDups, KeyOrder order,
                                                     DbFlags flags, std::uint32_t depth,
                                                     bool appendMode) {
    if (depth > kMaxTreeDepth) {
        return Status::Corruption("btree insert depth exceeded bound");
    }
    auto pageResult = pm_.getPage(nodePageId);
    if (!pageResult) return pageResult.status();
    const PageHeader* header = pageResult.value();
    if (header->type == PageType::Leaf) {
        LeafPageView view(header);
        std::vector<LeafEntry> entries = view.decodeAll();
        LeafEntry newEntry;
        newEntry.key = toByteBuffer(key);
        if (allowDups && value.size() > overflowThreshold()) {
            return Status::InvalidArgument(
                "DUPSORT values must fit inline (no overflow); value size exceeds threshold");
        }
        ByteBuffer compressScratch;
        ByteView storedValue = allowDups ? value : maybeCompress(value, compressScratch, newEntry);
        bool useOverflow = storedValue.size() > overflowThreshold();
        if (useOverflow) {
            auto ovResult = writeOverflowChain(txnId, storedValue);
            if (!ovResult) return ovResult.status();
            newEntry.isOverflow = true;
            newEntry.overflowFirstPage = ovResult.value();
            newEntry.overflowTotalLen = storedValue.size();
        } else {
            newEntry.value = toByteBuffer(storedValue);
        }

        if (appendMode) {
            // Caller (Cursor::put's Append flag / BTree::insertAppend)
            // guarantees `key` sorts after everything already in the tree;
            // re-verify against the actual rightmost entry here (we already
            // have it decoded) rather than trust the caller blindly -- an
            // InvalidArgument here is far better than a silently
            // out-of-order leaf.
            if (!entries.empty() && order.cmpKey(toByteView(entries.back().key), key) >= 0) {
                return Status::InvalidArgument(
                    "append key does not sort after the last key in the tree");
            }
            entries.push_back(newEntry);
        } else if (allowDups) {
            std::uint16_t existingIdx = view.lowerBound(key, order);
            bool keyExists = existingIdx < view.count() && order.cmpKey(view.keyAt(existingIdx), key) == 0;
            if (keyExists && view.isFixedRunAt(existingIdx)) {
                return Status::InvalidArgument(
                    "key already holds a DUPFIXED fixed-run; cannot mix with ordinary dup put "
                    "(use putMultiple)");
            }
            if (keyExists && view.isSubTreeAt(existingIdx)) {
                // Already escalated (see escalateDupGroupIfNeeded below):
                // insert the new value into its existing sub-tree instead of
                // the flat entries vector -- there is no flat cell to touch.
                PageId subRoot = view.subTreeRootAt(existingIdx);
                KeyOrder subOrder;
                subOrder.customKeyCmp = [order](ByteView a, ByteView b) { return order.cmpValue(a, b); };
                auto sr = insert(subRoot, txnId, value, ByteView{}, overwrite, /*allowDups=*/false,
                                 DbFlags::None, &subOrder);
                if (!sr) return sr.status();
                entries[existingIdx].subTreeRoot = sr.value();
            } else {
                if (hasFlag(flags, DbFlags::DupFixed) && keyExists) {
                    // Enforce fixed value size against any existing dup of this key.
                    if (!view.isOverflowAt(existingIdx)) {
                        ByteView existing = view.inlineValueAt(existingIdx);
                        if (existing.size() != value.size()) {
                            return Status::BadValSize(
                                "DUPFIXED value size mismatch: expected " +
                                std::to_string(existing.size()) + " got " +
                                std::to_string(value.size()));
                        }
                    }
                }
                // Order by (key, value). Exact match → KeyExists or replace.
                std::uint16_t idx = view.lowerBoundKeyValue(key, value, order);
                bool exact = false;
                if (idx < entries.size() &&
                    order.cmpKey(toByteView(entries[idx].key), key) == 0 &&
                    !entries[idx].isOverflow &&
                    order.cmpValue(toByteView(entries[idx].value), value) == 0) {
                    exact = true;
                }
                if (exact) {
                    if (!overwrite) return Status::KeyExists();  // MDB_NODUPDATA
                    entries[idx] = newEntry;
                } else {
                    entries.insert(entries.begin() + idx, newEntry);
                }
                // This key's dup-group may now be large enough to escalate
                // into its own sub-tree (LMDB-style nested dup storage).
                Status es = escalateDupGroupIfNeeded(entries, txnId, order,
                                                     core::payloadCapacity(pm_.pageSize()));
                if (!es.ok()) return es;
            }
        } else {
            std::uint16_t idx = view.lowerBound(key, order);
            bool exists = (idx < entries.size()) && order.cmpKey(toByteView(entries[idx].key), key) == 0;
            if (exists) {
                if (!overwrite) return Status::KeyExists();
                if (entries[idx].isOverflow) {
                    Status rs = retireOverflowChain(entries[idx].overflowFirstPage, txnId);
                    if (!rs.ok()) return rs;
                }
                entries[idx] = newEntry;
            } else {
                entries.insert(entries.begin() + idx, newEntry);
            }
        }
        // The old leaf page is superseded by whatever we build below
        // (either a single replacement page or a split pair); retire it so
        // its space becomes reclaimable once no reader can still see it.
        pm_.retirePage(nodePageId, txnId);
        std::uint32_t capacity = core::payloadCapacity(pm_.pageSize());
        if (encodedLeafSize(entries) <= capacity) {
            auto allocResult = pm_.allocatePage(txnId, PageType::Leaf);
            if (!allocResult) return allocResult.status();
            PageHeader* newHeader = allocResult.value();
            encodeLeaf(newHeader, capacity, entries);
            pm_.finalizePage(newHeader, newHeader->payloadSize, newHeader->itemCount);
            InsertOutcome outcome;
            outcome.leftPageId = newHeader->pageId;
            return Result<InsertOutcome>(outcome);
        }
        // Split by encoded size so mixed-length keys/values still produce two
        // halves that each fit. count/2 can leave one half over capacity.
        std::size_t mid = pickLeafSplitIndex(entries, capacity);
        if (mid == 0) {
            return Status::Corruption(
                "leaf split: no valid split point (possible oversized key)");
        }
        std::vector<LeafEntry> leftEntries(entries.begin(), entries.begin() + static_cast<long>(mid));
        std::vector<LeafEntry> rightEntries(entries.begin() + static_cast<long>(mid), entries.end());
        auto leftAlloc = pm_.allocatePage(txnId, PageType::Leaf);
        if (!leftAlloc) return leftAlloc.status();
        PageHeader* leftHeader = leftAlloc.value();
        if (!encodeLeaf(leftHeader, capacity, leftEntries)) {
            return Status::Internal("leaf split produced a left half that still does not fit");
        }
        pm_.finalizePage(leftHeader, leftHeader->payloadSize, leftHeader->itemCount);
        auto rightAlloc = pm_.allocatePage(txnId, PageType::Leaf);
        if (!rightAlloc) return rightAlloc.status();
        PageHeader* rightHeader = rightAlloc.value();
        if (!encodeLeaf(rightHeader, capacity, rightEntries)) {
            return Status::Internal("leaf split produced a right half that still does not fit");
        }
        pm_.finalizePage(rightHeader, rightHeader->payloadSize, rightHeader->itemCount);
        InsertOutcome outcome;
        outcome.leftPageId = leftHeader->pageId;
        outcome.rightPageId = rightHeader->pageId;
        outcome.splitKey = rightEntries.front().key;
        return Result<InsertOutcome>(outcome);
    }
    if (header->type == PageType::Branch) {
        BranchPageView view(header);
        std::uint16_t idx = appendMode ? static_cast<std::uint16_t>(view.count() - 1)
                                        : view.findChildIndex(key, order);
        PageId childId = view.childAt(idx);
        auto childResult = insertRecursive(childId, txnId, key, value, overwrite, allowDups, order, flags,
                                            depth + 1, appendMode);
        if (!childResult) return childResult.status();
        InsertOutcome childOutcome = childResult.value();
        std::vector<BranchEntry> entries = view.decodeAll();
        entries[idx].child = childOutcome.leftPageId;
        if (childOutcome.splitKey.has_value()) {
            BranchEntry newEntry;
            newEntry.key = *childOutcome.splitKey;
            newEntry.child = childOutcome.rightPageId;
            entries.insert(entries.begin() + idx + 1, newEntry);
        }
        // Old branch page is superseded regardless of split outcome.
        pm_.retirePage(nodePageId, txnId);
        std::uint32_t capacity = core::payloadCapacity(pm_.pageSize());
        if (encodedBranchSize(entries) <= capacity) {
            auto allocResult = pm_.allocatePage(txnId, PageType::Branch);
            if (!allocResult) return allocResult.status();
            PageHeader* newHeader = allocResult.value();
            encodeBranch(newHeader, capacity, entries);
            pm_.finalizePage(newHeader, newHeader->payloadSize, newHeader->itemCount);
            InsertOutcome outcome;
            outcome.leftPageId = newHeader->pageId;
            return Result<InsertOutcome>(outcome);
        }
        // Branch split by encoded size (same rationale as leaf).
        std::size_t mid = pickBranchSplitIndex(entries, capacity);
        if (mid == 0) {
            return Status::Corruption(
                "branch split: no valid split point (possible oversized key)");
        }
        ByteBuffer promotedKey = entries[mid].key;
        std::vector<BranchEntry> leftEntries(entries.begin(), entries.begin() + static_cast<long>(mid));
        std::vector<BranchEntry> rightEntries(entries.begin() + static_cast<long>(mid), entries.end());
        rightEntries.front().key.clear();
        auto leftAlloc = pm_.allocatePage(txnId, PageType::Branch);
        if (!leftAlloc) return leftAlloc.status();
        PageHeader* leftHeader = leftAlloc.value();
        if (!encodeBranch(leftHeader, capacity, leftEntries)) {
            return Status::Internal("branch split produced a left half that still does not fit");
        }
        pm_.finalizePage(leftHeader, leftHeader->payloadSize, leftHeader->itemCount);
        auto rightAlloc = pm_.allocatePage(txnId, PageType::Branch);
        if (!rightAlloc) return rightAlloc.status();
        PageHeader* rightHeader = rightAlloc.value();
        if (!encodeBranch(rightHeader, capacity, rightEntries)) {
            return Status::Internal("branch split produced a right half that still does not fit");
        }
        pm_.finalizePage(rightHeader, rightHeader->payloadSize, rightHeader->itemCount);
        InsertOutcome outcome;
        outcome.leftPageId = leftHeader->pageId;
        outcome.rightPageId = rightHeader->pageId;
        outcome.splitKey = promotedKey;
        return Result<InsertOutcome>(outcome);
    }
    return Status::Corruption("unexpected page type during insert");
}
Result<PageId> BTree::insert(PageId rootPageId, TxnId txnId, ByteView key, ByteView value,
                              bool overwrite, bool allowDups, DbFlags flags, const KeyOrder* customOrder) {
    flags = effectiveFlags(flags);
    if (hasFlag(flags, DbFlags::DupSort)) allowDups = true;
    KeyOrder order = customOrder ? *customOrder : KeyOrder::fromFlags(flags);
    // Keys are always inlined. Reject oversized keys before they can produce
    // single-entry pages that do not fit (and 0-entry branch collapse hazards).
    if (key.size() > maxKeySize()) {
        return Status::BadValSize(
            "key size " + std::to_string(key.size()) +
            " exceeds maximum " + std::to_string(maxKeySize()));
    }
    if (allowDups && value.size() > overflowThreshold()) {
        return Status::InvalidArgument(
            "DUPSORT values must fit inline; value exceeds overflow threshold");
    }
    if (rootPageId == kInvalidPageId) {
        std::vector<LeafEntry> entries(1);
        entries[0].key = toByteBuffer(key);
        ByteBuffer compressScratch;
        ByteView storedValue = allowDups ? value : maybeCompress(value, compressScratch, entries[0]);
        bool useOverflow = storedValue.size() > overflowThreshold();
        if (useOverflow) {
            auto ovResult = writeOverflowChain(txnId, storedValue);
            if (!ovResult) return ovResult.status();
            entries[0].isOverflow = true;
            entries[0].overflowFirstPage = ovResult.value();
            entries[0].overflowTotalLen = storedValue.size();
        } else {
            entries[0].value = toByteBuffer(storedValue);
        }
        auto allocResult = pm_.allocatePage(txnId, PageType::Leaf);
        if (!allocResult) return allocResult.status();
        PageHeader* header = allocResult.value();
        encodeLeaf(header, core::payloadCapacity(pm_.pageSize()), entries);
        pm_.finalizePage(header, header->payloadSize, header->itemCount);
        return Result<PageId>(header->pageId);
    }
    auto result = insertRecursive(rootPageId, txnId, key, value, overwrite, allowDups, order, flags);
    if (!result) return result.status();
    return finishInsertAtRoot(txnId, result.value());
}

Result<PageId> BTree::finishInsertAtRoot(TxnId txnId, const InsertOutcome& outcome) {
    if (!outcome.splitKey.has_value()) return Result<PageId>(outcome.leftPageId);
    // Root split: create a brand-new branch root with two children.
    std::vector<BranchEntry> rootEntries(2);
    rootEntries[0].key.clear();
    rootEntries[0].child = outcome.leftPageId;
    rootEntries[1].key = *outcome.splitKey;
    rootEntries[1].child = outcome.rightPageId;
    auto allocResult = pm_.allocatePage(txnId, PageType::Branch);
    if (!allocResult) return allocResult.status();
    PageHeader* header = allocResult.value();
    encodeBranch(header, core::payloadCapacity(pm_.pageSize()), rootEntries);
    pm_.finalizePage(header, header->payloadSize, header->itemCount);
    return Result<PageId>(header->pageId);
}

Result<PageId> BTree::insertAppend(PageId rootPageId, TxnId txnId, ByteView key, ByteView value,
                                    DbFlags flags) {
    flags = effectiveFlags(flags);
    KeyOrder order = KeyOrder::fromFlags(flags);
    if (key.size() > maxKeySize()) {
        return Status::BadValSize(
            "key size " + std::to_string(key.size()) +
            " exceeds maximum " + std::to_string(maxKeySize()));
    }
    if (rootPageId == kInvalidPageId) {
        // Empty tree: identical to BTree::insert's empty-tree branch.
        std::vector<LeafEntry> entries(1);
        entries[0].key = toByteBuffer(key);
        ByteBuffer compressScratch;
        ByteView storedValue = maybeCompress(value, compressScratch, entries[0]);
        bool useOverflow = storedValue.size() > overflowThreshold();
        if (useOverflow) {
            auto ovResult = writeOverflowChain(txnId, storedValue);
            if (!ovResult) return ovResult.status();
            entries[0].isOverflow = true;
            entries[0].overflowFirstPage = ovResult.value();
            entries[0].overflowTotalLen = storedValue.size();
        } else {
            entries[0].value = toByteBuffer(storedValue);
        }
        auto allocResult = pm_.allocatePage(txnId, PageType::Leaf);
        if (!allocResult) return allocResult.status();
        PageHeader* header = allocResult.value();
        encodeLeaf(header, core::payloadCapacity(pm_.pageSize()), entries);
        pm_.finalizePage(header, header->payloadSize, header->itemCount);
        return Result<PageId>(header->pageId);
    }
    auto result = insertRecursive(rootPageId, txnId, key, value, /*overwrite=*/false, /*allowDups=*/false,
                                   order, flags, /*depth=*/0, /*appendMode=*/true);
    if (!result) return result.status();
    return finishInsertAtRoot(txnId, result.value());
}

Result<BTree::InsertOutcome> BTree::insertMultipleRecursive(PageId nodePageId, TxnId txnId, ByteView key,
                                                             std::uint32_t elemSize, ByteView valuesBlob,
                                                             KeyOrder order, std::uint32_t depth) {
    if (depth > kMaxTreeDepth) {
        return Status::Corruption("btree insert depth exceeded bound");
    }
    auto pageResult = pm_.getPage(nodePageId);
    if (!pageResult) return pageResult.status();
    const PageHeader* header = pageResult.value();
    if (header->type == PageType::Leaf) {
        LeafPageView view(header);
        std::vector<LeafEntry> entries = view.decodeAll();
        std::uint16_t idx = view.lowerBound(key, order);
        bool exists = (idx < entries.size()) && order.cmpKey(toByteView(entries[idx].key), key) == 0;

        LeafEntry newEntry;
        newEntry.key = toByteBuffer(key);
        newEntry.fixedElemSize = elemSize;

        if (exists) {
            const LeafEntry& old = entries[idx];
            if (old.isOverflow || old.fixedElemSize == 0) {
                return Status::InvalidArgument(
                    "key already holds non-DUPFIXED dup data; cannot mix with putMultiple");
            }
            if (old.fixedElemSize != elemSize) {
                return Status::InvalidArgument(
                    "putMultiple element size " + std::to_string(elemSize) +
                    " does not match existing run's element size " + std::to_string(old.fixedElemSize));
            }
            // Append: caller (Cursor::putMultiple) is responsible for the
            // overall sort-order guarantee across the whole call sequence,
            // mirroring MDB_APPENDDUP + MDB_MULTIPLE used together.
            newEntry.value = old.value;
            newEntry.value.insert(newEntry.value.end(), valuesBlob.begin(), valuesBlob.end());
            entries[idx] = newEntry;
        } else {
            newEntry.value = toByteBuffer(valuesBlob);
            entries.insert(entries.begin() + idx, newEntry);
        }

        pm_.retirePage(nodePageId, txnId);
        std::uint32_t capacity = core::payloadCapacity(pm_.pageSize());
        if (encodedLeafSize(entries) <= capacity) {
            auto allocResult = pm_.allocatePage(txnId, PageType::Leaf);
            if (!allocResult) return allocResult.status();
            PageHeader* newHeader = allocResult.value();
            encodeLeaf(newHeader, capacity, entries);
            pm_.finalizePage(newHeader, newHeader->payloadSize, newHeader->itemCount);
            InsertOutcome outcome;
            outcome.leftPageId = newHeader->pageId;
            return Result<InsertOutcome>(outcome);
        }
        std::size_t mid = pickLeafSplitIndex(entries, capacity);
        if (mid == 0) {
            return Status::Corruption(
                "leaf split: no valid split point (possible oversized key/run)");
        }
        std::vector<LeafEntry> leftEntries(entries.begin(), entries.begin() + static_cast<long>(mid));
        std::vector<LeafEntry> rightEntries(entries.begin() + static_cast<long>(mid), entries.end());
        auto leftAlloc = pm_.allocatePage(txnId, PageType::Leaf);
        if (!leftAlloc) return leftAlloc.status();
        PageHeader* leftHeader = leftAlloc.value();
        if (!encodeLeaf(leftHeader, capacity, leftEntries)) {
            return Status::Internal("leaf split produced a left half that still does not fit");
        }
        pm_.finalizePage(leftHeader, leftHeader->payloadSize, leftHeader->itemCount);
        auto rightAlloc = pm_.allocatePage(txnId, PageType::Leaf);
        if (!rightAlloc) return rightAlloc.status();
        PageHeader* rightHeader = rightAlloc.value();
        if (!encodeLeaf(rightHeader, capacity, rightEntries)) {
            return Status::Internal("leaf split produced a right half that still does not fit");
        }
        pm_.finalizePage(rightHeader, rightHeader->payloadSize, rightHeader->itemCount);
        InsertOutcome outcome;
        outcome.leftPageId = leftHeader->pageId;
        outcome.rightPageId = rightHeader->pageId;
        outcome.splitKey = rightEntries.front().key;
        return Result<InsertOutcome>(outcome);
    }
    if (header->type == PageType::Branch) {
        BranchPageView view(header);
        std::uint16_t idx = view.findChildIndex(key, order);
        PageId childId = view.childAt(idx);
        auto childResult = insertMultipleRecursive(childId, txnId, key, elemSize, valuesBlob, order, depth + 1);
        if (!childResult) return childResult.status();
        InsertOutcome childOutcome = childResult.value();
        std::vector<BranchEntry> entries = view.decodeAll();
        entries[idx].child = childOutcome.leftPageId;
        if (childOutcome.splitKey.has_value()) {
            BranchEntry newBranchEntry;
            newBranchEntry.key = *childOutcome.splitKey;
            newBranchEntry.child = childOutcome.rightPageId;
            entries.insert(entries.begin() + idx + 1, newBranchEntry);
        }
        pm_.retirePage(nodePageId, txnId);
        std::uint32_t capacity = core::payloadCapacity(pm_.pageSize());
        if (encodedBranchSize(entries) <= capacity) {
            auto allocResult = pm_.allocatePage(txnId, PageType::Branch);
            if (!allocResult) return allocResult.status();
            PageHeader* newHeader = allocResult.value();
            encodeBranch(newHeader, capacity, entries);
            pm_.finalizePage(newHeader, newHeader->payloadSize, newHeader->itemCount);
            InsertOutcome outcome;
            outcome.leftPageId = newHeader->pageId;
            return Result<InsertOutcome>(outcome);
        }
        std::size_t mid = pickBranchSplitIndex(entries, capacity);
        if (mid == 0) {
            return Status::Corruption(
                "branch split: no valid split point (possible oversized key)");
        }
        ByteBuffer promotedKey = entries[mid].key;
        std::vector<BranchEntry> leftEntries(entries.begin(), entries.begin() + static_cast<long>(mid));
        std::vector<BranchEntry> rightEntries(entries.begin() + static_cast<long>(mid), entries.end());
        rightEntries.front().key.clear();
        auto leftAlloc = pm_.allocatePage(txnId, PageType::Branch);
        if (!leftAlloc) return leftAlloc.status();
        PageHeader* leftHeader = leftAlloc.value();
        if (!encodeBranch(leftHeader, capacity, leftEntries)) {
            return Status::Internal("branch split produced a left half that still does not fit");
        }
        pm_.finalizePage(leftHeader, leftHeader->payloadSize, leftHeader->itemCount);
        auto rightAlloc = pm_.allocatePage(txnId, PageType::Branch);
        if (!rightAlloc) return rightAlloc.status();
        PageHeader* rightHeader = rightAlloc.value();
        if (!encodeBranch(rightHeader, capacity, rightEntries)) {
            return Status::Internal("branch split produced a right half that still does not fit");
        }
        pm_.finalizePage(rightHeader, rightHeader->payloadSize, rightHeader->itemCount);
        InsertOutcome outcome;
        outcome.leftPageId = leftHeader->pageId;
        outcome.rightPageId = rightHeader->pageId;
        outcome.splitKey = promotedKey;
        return Result<InsertOutcome>(outcome);
    }
    return Status::Corruption("unexpected page type during insert");
}

Result<PageId> BTree::insertMultiple(PageId rootPageId, TxnId txnId, ByteView key, std::uint32_t elemSize,
                                     ByteView valuesBlob, DbFlags flags) {
    flags = effectiveFlags(flags);
    KeyOrder order = KeyOrder::fromFlags(flags);
    if (key.size() > maxKeySize()) {
        return Status::BadValSize(
            "key size " + std::to_string(key.size()) +
            " exceeds maximum " + std::to_string(maxKeySize()));
    }
    if (elemSize == 0) return Status::InvalidArgument("putMultiple element size must be non-zero");
    if (valuesBlob.size() % elemSize != 0) {
        return Status::InvalidArgument("putMultiple values blob size is not a multiple of elemSize");
    }
    if (elemSize > overflowThreshold()) {
        return Status::InvalidArgument(
            "putMultiple element size exceeds overflow threshold; DUPFIXED values must be small and inline");
    }
    if (rootPageId == kInvalidPageId) {
        std::vector<LeafEntry> entries(1);
        entries[0].key = toByteBuffer(key);
        entries[0].fixedElemSize = elemSize;
        entries[0].value = toByteBuffer(valuesBlob);
        auto allocResult = pm_.allocatePage(txnId, PageType::Leaf);
        if (!allocResult) return allocResult.status();
        PageHeader* header = allocResult.value();
        encodeLeaf(header, core::payloadCapacity(pm_.pageSize()), entries);
        pm_.finalizePage(header, header->payloadSize, header->itemCount);
        return Result<PageId>(header->pageId);
    }
    auto result = insertMultipleRecursive(rootPageId, txnId, key, elemSize, valuesBlob, order, 0);
    if (!result) return result.status();
    return finishInsertAtRoot(txnId, result.value());
}
// ---------------------------------------------------------------------------
// Write path: remove
// ---------------------------------------------------------------------------
namespace {
// Pick a split index in [1, n-1] such that both halves fit in `capacity` and
// the encoded-size imbalance is minimized. Count-based n/2 is wrong for
// mixed-size entries (a legitimate insert can fail encodeLeaf on one half
// even though a nearby split point would succeed).
std::size_t pickLeafSplitIndex(const std::vector<LeafEntry>& entries, std::uint32_t capacity) {
    const std::size_t n = entries.size();
    if (n < 2) return 0;  // cannot split
    std::size_t best = 0;
    std::size_t bestDiff = static_cast<std::size_t>(-1);
    for (std::size_t mid = 1; mid < n; ++mid) {
        std::vector<LeafEntry> left(entries.begin(),
                                    entries.begin() + static_cast<long>(mid));
        std::vector<LeafEntry> right(entries.begin() + static_cast<long>(mid),
                                     entries.end());
        const std::size_t ls = encodedLeafSize(left);
        const std::size_t rs = encodedLeafSize(right);
        if (ls > capacity || rs > capacity) continue;
        const std::size_t diff = ls > rs ? ls - rs : rs - ls;
        if (diff < bestDiff) {
            bestDiff = diff;
            best = mid;
        }
    }
    return best;  // 0 => no valid split
}

std::size_t pickBranchSplitIndex(const std::vector<BranchEntry>& entries, std::uint32_t capacity) {
    const std::size_t n = entries.size();
    if (n < 2) return 0;
    std::size_t best = 0;
    std::size_t bestDiff = static_cast<std::size_t>(-1);
    for (std::size_t mid = 1; mid < n; ++mid) {
        // Branch split promotes entries[mid].key; right half starts at mid with
        // its key cleared to "-infinity". Size the right half the same way encode
        // will see it.
        std::vector<BranchEntry> left(entries.begin(),
                                      entries.begin() + static_cast<long>(mid));
        std::vector<BranchEntry> right(entries.begin() + static_cast<long>(mid),
                                       entries.end());
        if (!right.empty()) right.front().key.clear();
        const std::size_t ls = encodedBranchSize(left);
        const std::size_t rs = encodedBranchSize(right);
        if (ls > capacity || rs > capacity) continue;
        const std::size_t diff = ls > rs ? ls - rs : rs - ls;
        if (diff < bestDiff) {
            bestDiff = diff;
            best = mid;
        }
    }
    return best;
}
} // namespace
Status BTree::rebalanceLeafChild(std::vector<BranchEntry>& entries, std::size_t childIdx, TxnId txnId,
                                  std::vector<LeafEntry>&& childEntries) {
    bool useRight = (childIdx + 1 < entries.size());
    std::size_t siblingIdx = useRight ? childIdx + 1 : childIdx - 1;
    auto sibPageResult = pm_.getPage(entries[siblingIdx].child);
    if (!sibPageResult) return sibPageResult.status();
    const PageHeader* sibHeader = sibPageResult.value();
    if (sibHeader->type != PageType::Leaf) return Status::Corruption("sibling type mismatch during leaf rebalance");
    LeafPageView sibView(sibHeader);
    std::vector<LeafEntry> sibEntries = sibView.decodeAll();
    pm_.retirePage(entries[siblingIdx].child, txnId);
    std::vector<LeafEntry> combined;
    combined.reserve(childEntries.size() + sibEntries.size());
    if (useRight) {
        for (auto& e : childEntries) combined.push_back(std::move(e));
        for (auto& e : sibEntries) combined.push_back(std::move(e));
    } else {
        for (auto& e : sibEntries) combined.push_back(std::move(e));
        for (auto& e : childEntries) combined.push_back(std::move(e));
    }
    std::uint32_t capacity = core::payloadCapacity(pm_.pageSize());
    std::size_t leftSlot = useRight ? childIdx : siblingIdx;
    std::size_t rightSlot = useRight ? siblingIdx : childIdx;
    if (encodedLeafSize(combined) <= capacity) {
        // Merge: one node absorbs the other entirely.
        auto allocResult = pm_.allocatePage(txnId, PageType::Leaf);
        if (!allocResult) return allocResult.status();
        PageHeader* header = allocResult.value();
        if (!encodeLeaf(header, capacity, combined)) return Status::Internal("leaf merge unexpectedly overflowed");
        pm_.finalizePage(header, header->payloadSize, header->itemCount);
        ByteBuffer survivingKey = entries[leftSlot].key; // leftmost of the pair keeps its original key
        entries.erase(entries.begin() + static_cast<long>(std::max(leftSlot, rightSlot)));
        entries.erase(entries.begin() + static_cast<long>(std::min(leftSlot, rightSlot)));
        BranchEntry mergedEntry;
        mergedEntry.key = std::move(survivingKey);
        mergedEntry.child = header->pageId;
        entries.insert(entries.begin() + static_cast<long>(std::min(leftSlot, rightSlot)), mergedEntry);
        return Status::OK();
    }
    // Redistribute by encoded size (not entry count).
    std::size_t mid = pickLeafSplitIndex(combined, capacity);
    if (mid == 0) {
        return Status::Corruption("leaf redistribute: no valid split point");
    }
    std::vector<LeafEntry> leftEntries(std::make_move_iterator(combined.begin()),
                                        std::make_move_iterator(combined.begin() + static_cast<long>(mid)));
    std::vector<LeafEntry> rightEntries(std::make_move_iterator(combined.begin() + static_cast<long>(mid)),
                                          std::make_move_iterator(combined.end()));
    auto leftAlloc = pm_.allocatePage(txnId, PageType::Leaf);
    if (!leftAlloc) return leftAlloc.status();
    PageHeader* leftHeader = leftAlloc.value();
    if (!encodeLeaf(leftHeader, capacity, leftEntries)) return Status::Internal("leaf redistribution left half overflowed");
    pm_.finalizePage(leftHeader, leftHeader->payloadSize, leftHeader->itemCount);
    auto rightAlloc = pm_.allocatePage(txnId, PageType::Leaf);
    if (!rightAlloc) return rightAlloc.status();
    PageHeader* rightHeader = rightAlloc.value();
    if (!encodeLeaf(rightHeader, capacity, rightEntries)) return Status::Internal("leaf redistribution right half overflowed");
    pm_.finalizePage(rightHeader, rightHeader->payloadSize, rightHeader->itemCount);
    entries[leftSlot].child = leftHeader->pageId; // key at leftSlot unchanged (preserves its range start)
    entries[rightSlot].child = rightHeader->pageId;
    entries[rightSlot].key = rightEntries.front().key; // new boundary between the two siblings
    return Status::OK();
}
Status BTree::rebalanceBranchChild(std::vector<BranchEntry>& entries, std::size_t childIdx, TxnId txnId,
                                    std::vector<BranchEntry>&& childEntries) {
    bool useRight = (childIdx + 1 < entries.size());
    std::size_t siblingIdx = useRight ? childIdx + 1 : childIdx - 1;
    auto sibPageResult = pm_.getPage(entries[siblingIdx].child);
    if (!sibPageResult) return sibPageResult.status();
    const PageHeader* sibHeader = sibPageResult.value();
    if (sibHeader->type != PageType::Branch) return Status::Corruption("sibling type mismatch during branch rebalance");
    BranchPageView sibView(sibHeader);
    std::vector<BranchEntry> sibEntries = sibView.decodeAll();
    pm_.retirePage(entries[siblingIdx].child, txnId);
    // The separator that used to sit between the two siblings in the parent
    // must become a *real* key on whichever side's former "-infinity" first
    // entry now lands in the middle of the concatenated list.
    ByteBuffer separator = useRight ? entries[siblingIdx].key : entries[childIdx].key;
    std::vector<BranchEntry> combined;
    combined.reserve(childEntries.size() + sibEntries.size());
    if (useRight) {
        for (auto& e : childEntries) combined.push_back(std::move(e));
        sibEntries.front().key = separator;
        for (auto& e : sibEntries) combined.push_back(std::move(e));
    } else {
        sibEntries.front().key = separator;
        for (auto& e : sibEntries) combined.push_back(std::move(e));
        for (auto& e : childEntries) combined.push_back(std::move(e));
    }
    std::uint32_t capacity = core::payloadCapacity(pm_.pageSize());
    std::size_t leftSlot = useRight ? childIdx : siblingIdx;
    std::size_t rightSlot = useRight ? siblingIdx : childIdx;
    if (encodedBranchSize(combined) <= capacity) {
        auto allocResult = pm_.allocatePage(txnId, PageType::Branch);
        if (!allocResult) return allocResult.status();
        PageHeader* header = allocResult.value();
        if (!encodeBranch(header, capacity, combined)) return Status::Internal("branch merge unexpectedly overflowed");
        pm_.finalizePage(header, header->payloadSize, header->itemCount);
        ByteBuffer survivingKey = entries[leftSlot].key;
        entries.erase(entries.begin() + static_cast<long>(std::max(leftSlot, rightSlot)));
        entries.erase(entries.begin() + static_cast<long>(std::min(leftSlot, rightSlot)));
        BranchEntry mergedEntry;
        mergedEntry.key = std::move(survivingKey);
        mergedEntry.child = header->pageId;
        entries.insert(entries.begin() + static_cast<long>(std::min(leftSlot, rightSlot)), mergedEntry);
        return Status::OK();
    }
    std::size_t mid = pickBranchSplitIndex(combined, capacity);
    if (mid == 0) {
        return Status::Corruption("branch redistribute: no valid split point");
    }
    ByteBuffer promotedKey = combined[mid].key;
    std::vector<BranchEntry> leftEntries(std::make_move_iterator(combined.begin()),
                                           std::make_move_iterator(combined.begin() + static_cast<long>(mid)));
    std::vector<BranchEntry> rightEntries(std::make_move_iterator(combined.begin() + static_cast<long>(mid)),
                                            std::make_move_iterator(combined.end()));
    rightEntries.front().key.clear();
    auto leftAlloc = pm_.allocatePage(txnId, PageType::Branch);
    if (!leftAlloc) return leftAlloc.status();
    PageHeader* leftHeader = leftAlloc.value();
    if (!encodeBranch(leftHeader, capacity, leftEntries)) return Status::Internal("branch redistribution left half overflowed");
    pm_.finalizePage(leftHeader, leftHeader->payloadSize, leftHeader->itemCount);
    auto rightAlloc = pm_.allocatePage(txnId, PageType::Branch);
    if (!rightAlloc) return rightAlloc.status();
    PageHeader* rightHeader = rightAlloc.value();
    if (!encodeBranch(rightHeader, capacity, rightEntries)) return Status::Internal("branch redistribution right half overflowed");
    pm_.finalizePage(rightHeader, rightHeader->payloadSize, rightHeader->itemCount);
    entries[leftSlot].child = leftHeader->pageId;
    entries[rightSlot].child = rightHeader->pageId;
    entries[rightSlot].key = promotedKey;
    return Status::OK();
}
Result<BTree::RemoveOutcome> BTree::removeRecursive(PageId nodePageId, TxnId txnId, ByteView key,
                                                     const ByteView* matchValue, KeyOrder order,
                                                     std::uint32_t depth) {
    if (depth > kMaxTreeDepth) {
        return Status::Corruption("btree remove depth exceeded bound");
    }
    auto pageResult = pm_.getPage(nodePageId);
    if (!pageResult) return pageResult.status();
    const PageHeader* header = pageResult.value();
    if (header->type == PageType::Leaf) {
        LeafPageView view(header);
        std::uint16_t idx = view.lowerBound(key, order);
        if (idx >= view.count() || order.cmpKey(view.keyAt(idx), key) != 0) return Status::NotFound();
        std::vector<LeafEntry> entries = view.decodeAll();
        if (matchValue != nullptr) {
            if (view.isFixedRunAt(idx)) {
                return Status::InvalidArgument(
                    "removeOne is not supported on a DUPFIXED fixed-run key; use remove() to delete the whole run");
            }
            if (view.isSubTreeAt(idx)) {
                // Remove the single value from the existing sub-tree instead
                // of touching the flat entries vector -- there is no flat
                // cell for this key.
                KeyOrder subOrder;
                subOrder.customKeyCmp = [order](ByteView a, ByteView b) { return order.cmpValue(a, b); };
                PageId subRoot = view.subTreeRootAt(idx);
                auto sr = removeOne(subRoot, txnId, *matchValue, ByteView{}, DbFlags::None, &subOrder);
                if (!sr) return sr.status();
                if (sr.value() == kInvalidPageId) {
                    // Sub-tree is now empty -- the key itself must disappear
                    // entirely (a DUPSORT key with zero values doesn't
                    // exist), matching the flat-storage case erasing the
                    // last dup's own entry.
                    entries.erase(entries.begin() + idx);
                } else {
                    entries[idx].subTreeRoot = sr.value();
                }
            } else {
                // Remove single (key, value) under DUPSORT.
                idx = view.lowerBoundKeyValue(key, *matchValue, order);
                if (idx >= entries.size() ||
                    order.cmpKey(toByteView(entries[idx].key), key) != 0 ||
                    entries[idx].isOverflow ||
                    order.cmpValue(toByteView(entries[idx].value), *matchValue) != 0) {
                    return Status::NotFound();
                }
                entries.erase(entries.begin() + idx);
            }
        } else {
            // Remove all dups of key.
            std::uint16_t n = 0;
            while (idx + n < entries.size() &&
                   order.cmpKey(toByteView(entries[idx + n].key), key) == 0) {
                if (entries[idx + n].isOverflow) {
                    Status rs = retireOverflowChain(entries[idx + n].overflowFirstPage, txnId);
                    if (!rs.ok()) return rs;
                } else if (entries[idx + n].isSubTree) {
                    Status rs = retireSubTree(entries[idx + n].subTreeRoot, txnId);
                    if (!rs.ok()) return rs;
                }
                ++n;
            }
            entries.erase(entries.begin() + idx, entries.begin() + idx + n);
        }
        pm_.retirePage(nodePageId, txnId);
        RemoveOutcome outcome;
        outcome.nodeType = PageType::Leaf;
        if (entries.empty()) {
            outcome.kind = RemoveKind::Empty;
            return Result<RemoveOutcome>(outcome);
        }
        std::uint32_t capacity = core::payloadCapacity(pm_.pageSize());
        std::size_t encoded = encodedLeafSize(entries);
        if (encoded < minFillThreshold()) {
            outcome.kind = RemoveKind::Underflow;
            outcome.leafEntries = std::move(entries);
            return Result<RemoveOutcome>(outcome);
        }
        auto allocResult = pm_.allocatePage(txnId, PageType::Leaf);
        if (!allocResult) return allocResult.status();
        PageHeader* newHeader = allocResult.value();
        encodeLeaf(newHeader, capacity, entries);
        pm_.finalizePage(newHeader, newHeader->payloadSize, newHeader->itemCount);
        outcome.kind = RemoveKind::Ok;
        outcome.pageId = newHeader->pageId;
        return Result<RemoveOutcome>(outcome);
    }
    if (header->type == PageType::Branch) {
        BranchPageView view(header);
        std::uint16_t idx = view.findChildIndex(key, order);
        PageId childId = view.childAt(idx);
        auto childResult = removeRecursive(childId, txnId, key, matchValue, order, depth + 1);
        if (!childResult) return childResult.status();
        RemoveOutcome childOutcome = childResult.value();
        std::vector<BranchEntry> entries = view.decodeAll();
        pm_.retirePage(nodePageId, txnId);
        switch (childOutcome.kind) {
            case RemoveKind::Ok:
                entries[idx].child = childOutcome.pageId;
                break;
            case RemoveKind::Empty:
                entries.erase(entries.begin() + idx);
                if (idx == 0 && !entries.empty()) entries[0].key.clear();
                break;
            case RemoveKind::Underflow: {
                Status rs = (childOutcome.nodeType == PageType::Leaf)
                                 ? rebalanceLeafChild(entries, idx, txnId, std::move(childOutcome.leafEntries))
                                 : rebalanceBranchChild(entries, idx, txnId, std::move(childOutcome.branchEntries));
                if (!rs.ok()) return rs;
                break;
            }
        }
        RemoveOutcome outcome;
        outcome.nodeType = PageType::Branch;
        if (entries.empty()) {
            outcome.kind = RemoveKind::Empty;
            return Result<RemoveOutcome>(outcome);
        }
        if (entries.size() == 1) {
            // Single-child branch is redundant; collapse it away. The tree
            // becomes shallower along this path, which is fine since every
            // traversal dispatches dynamically on page type rather than
            // assuming uniform depth. entries[0].child is always a real,
            // already-persisted page at this point (Ok/merge/redistribute
            // all produce persisted pages before we get here).
            outcome.kind = RemoveKind::Ok;
            outcome.pageId = entries[0].child;
            return Result<RemoveOutcome>(outcome);
        }
        std::uint32_t capacity = core::payloadCapacity(pm_.pageSize());
        std::size_t encoded = encodedBranchSize(entries);
        if (encoded < minFillThreshold()) {
            outcome.kind = RemoveKind::Underflow;
            outcome.branchEntries = std::move(entries);
            return Result<RemoveOutcome>(outcome);
        }
        auto allocResult = pm_.allocatePage(txnId, PageType::Branch);
        if (!allocResult) return allocResult.status();
        PageHeader* newHeader = allocResult.value();
        encodeBranch(newHeader, capacity, entries);
        pm_.finalizePage(newHeader, newHeader->payloadSize, newHeader->itemCount);
        outcome.kind = RemoveKind::Ok;
        outcome.pageId = newHeader->pageId;
        return Result<RemoveOutcome>(outcome);
    }
    return Status::Corruption("unexpected page type during remove");
}
Result<PageId> BTree::remove(PageId rootPageId, TxnId txnId, ByteView key, DbFlags flags,
                              const KeyOrder* customOrder) {
    if (rootPageId == kInvalidPageId) return Status::NotFound();
    KeyOrder order = customOrder ? *customOrder : KeyOrder::fromFlags(flags);
    auto result = removeRecursive(rootPageId, txnId, key, nullptr, order);
    if (!result) return result.status();
    RemoveOutcome outcome = result.value();
    switch (outcome.kind) {
        case RemoveKind::Empty:
            return Result<PageId>(kInvalidPageId);
        case RemoveKind::Ok:
            return Result<PageId>(outcome.pageId);
        case RemoveKind::Underflow: {
            // The root has no sibling to rebalance against and is allowed
            // to sit below the minimum fill threshold; just persist it.
            std::uint32_t capacity = core::payloadCapacity(pm_.pageSize());
            if (outcome.nodeType == PageType::Leaf) {
                auto allocResult = pm_.allocatePage(txnId, PageType::Leaf);
                if (!allocResult) return allocResult.status();
                PageHeader* header = allocResult.value();
                encodeLeaf(header, capacity, outcome.leafEntries);
                pm_.finalizePage(header, header->payloadSize, header->itemCount);
                return Result<PageId>(header->pageId);
            }
            auto allocResult = pm_.allocatePage(txnId, PageType::Branch);
            if (!allocResult) return allocResult.status();
            PageHeader* header = allocResult.value();
            encodeBranch(header, capacity, outcome.branchEntries);
            pm_.finalizePage(header, header->payloadSize, header->itemCount);
            return Result<PageId>(header->pageId);
        }
    }
    return Status::Internal("unreachable RemoveKind");
}

Result<PageId> BTree::removeOne(PageId rootPageId, TxnId txnId, ByteView key, ByteView value,
                                 DbFlags flags, const KeyOrder* customOrder) {
    if (rootPageId == kInvalidPageId) return Status::NotFound();
    KeyOrder order = customOrder ? *customOrder : KeyOrder::fromFlags(flags);
    auto result = removeRecursive(rootPageId, txnId, key, &value, order);
    if (!result) return result.status();
    RemoveOutcome outcome = result.value();
    switch (outcome.kind) {
        case RemoveKind::Empty:
            return Result<PageId>(kInvalidPageId);
        case RemoveKind::Ok:
            return Result<PageId>(outcome.pageId);
        case RemoveKind::Underflow: {
            std::uint32_t capacity = core::payloadCapacity(pm_.pageSize());
            if (outcome.nodeType == PageType::Leaf) {
                auto allocResult = pm_.allocatePage(txnId, PageType::Leaf);
                if (!allocResult) return allocResult.status();
                PageHeader* header = allocResult.value();
                encodeLeaf(header, capacity, outcome.leafEntries);
                pm_.finalizePage(header, header->payloadSize, header->itemCount);
                return Result<PageId>(header->pageId);
            }
            auto allocResult = pm_.allocatePage(txnId, PageType::Branch);
            if (!allocResult) return allocResult.status();
            PageHeader* header = allocResult.value();
            encodeBranch(header, capacity, outcome.branchEntries);
            pm_.finalizePage(header, header->payloadSize, header->itemCount);
            return Result<PageId>(header->pageId);
        }
    }
    return Status::Internal("unreachable RemoveKind");
}

Result<std::vector<ByteBuffer>> BTree::getAll(PageId rootPageId, ByteView key, DbFlags flags,
                                              const KeyOrder* customOrder) const {
    KeyOrder order = customOrder ? *customOrder : KeyOrder::fromFlags(flags);
    std::vector<ByteBuffer> out;
    if (rootPageId == kInvalidPageId) return Status::NotFound();
    PageId cur = rootPageId;
    BoundedWalk depth(kMaxTreeDepth);
    while (true) {
        Status ds = depth.step("btree getAll depth");
        if (!ds.ok()) return ds;
        auto pageResult = pm_.getPage(cur);
        if (!pageResult) return pageResult.status();
        const PageHeader* header = pageResult.value();
        if (header->type == PageType::Leaf) {
            LeafPageView view(header);
            std::uint16_t idx = view.lowerBound(key, order);
            while (idx < view.count() && order.cmpKey(view.keyAt(idx), key) == 0) {
                if (view.isSubTreeAt(idx)) {
                    auto vals = collectSubTreeValues(view.subTreeRootAt(idx));
                    if (!vals) return vals.status();
                    for (auto& v : vals.value()) out.push_back(std::move(v));
                } else if (view.isFixedRunAt(idx)) {
                    ByteView run = view.fixedRunBytesAt(idx);
                    std::uint32_t elemSize = view.fixedRunElemSizeAt(idx);
                    if (elemSize > 0) {
                        for (std::size_t off = 0; off + elemSize <= run.size(); off += elemSize) {
                            out.push_back(toByteBuffer(ByteView(run.data() + off, elemSize)));
                        }
                    }
                } else {
                    LeafEntry entry = view.entryAt(idx);
                    auto mat = materializeValue(entry);
                    if (!mat) return mat.status();
                    out.push_back(mat.moveValue());
                }
                ++idx;
            }
            if (out.empty()) return Status::NotFound();
            return Result<std::vector<ByteBuffer>>(std::move(out));
        } else if (header->type == PageType::Branch) {
            BranchPageView view(header);
            std::uint16_t idx = view.findChildIndex(key, order);
            cur = view.childAt(idx);
        } else {
            return Status::Corruption("unexpected page type while traversing btree");
        }
    }
}

} // namespace annpocketdb::btree

