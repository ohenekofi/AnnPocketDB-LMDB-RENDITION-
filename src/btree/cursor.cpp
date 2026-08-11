#include "annpocketdb/btree/cursor.hpp"
#include "annpocketdb/btree/btree.hpp"
#include "annpocketdb/util/bounded_walk.hpp"

namespace annpocketdb::btree {

using core::PageHeader;
using core::PageType;

const core::PageManager& Cursor::pageManager() const { return tree_.pageManager(); }

Status Cursor::checkGeneration() const {
    if (writeGenerationAccessor_ && writeGenerationAccessor_() != capturedGeneration_) {
        return Status::BadTxn(
            "cursor invalidated by transaction move/destroy or nested transaction abort");
    }
    return Status::OK();
}

bool Cursor::isCurrentFixedRun() const {
    if (stack_.empty()) return false;
    const Frame& top = stack_.back();
    auto pr = pageManager().getPage(top.pageId);
    if (!pr) return false;
    LeafPageView v(pr.value());
    if (top.index >= v.count()) return false;
    return v.isFixedRunAt(top.index);
}

bool Cursor::isCurrentSubTree() const {
    if (stack_.empty()) return false;
    const Frame& top = stack_.back();
    auto pr = pageManager().getPage(top.pageId);
    if (!pr) return false;
    LeafPageView v(pr.value());
    if (top.index >= v.count()) return false;
    return v.isSubTreeAt(top.index);
}

Status Cursor::ensureSubTreeValuesCached() const {
    if (stack_.empty()) return Status::OK();
    Frame& top = stack_.back();
    if (top.subTreeValues) return Status::OK();
    auto pr = pageManager().getPage(top.pageId);
    if (!pr) return pr.status();
    LeafPageView v(pr.value());
    if (top.index >= v.count() || !v.isSubTreeAt(top.index)) return Status::OK();
    auto vals = tree_.collectSubTreeValues(v.subTreeRootAt(top.index));
    if (!vals) return vals.status();
    top.subTreeValues = std::make_shared<std::vector<ByteBuffer>>(vals.moveValue());
    return Status::OK();
}

Status Cursor::descendAppendFirst(PageId pageId) {
    PageId cur = pageId;
    BoundedWalk depth(kMaxTreeDepth);
    while (true) {
        Status ds = depth.step("cursor descend first");
        if (!ds.ok()) return ds;
        auto pr = pageManager().getPage(cur);
        if (!pr) return pr.status();
        const PageHeader* h = pr.value();
        if (h->type == PageType::Leaf) {
            LeafPageView v(h);
            stack_.push_back(Frame{cur, 0, v.count()});
            return Status::OK();
        }
        if (h->type == PageType::Branch) {
            BranchPageView v(h);
            stack_.push_back(Frame{cur, 0, v.count()});
            cur = v.childAt(0);
            continue;
        }
        return Status::Corruption("unexpected page type during cursor descent");
    }
}

Status Cursor::descendAppendLast(PageId pageId) {
    PageId cur = pageId;
    BoundedWalk depth(kMaxTreeDepth);
    while (true) {
        Status ds = depth.step("cursor descend last");
        if (!ds.ok()) return ds;
        auto pr = pageManager().getPage(cur);
        if (!pr) return pr.status();
        const PageHeader* h = pr.value();
        if (h->type == PageType::Leaf) {
            LeafPageView v(h);
            std::uint16_t idx = v.count() > 0 ? static_cast<std::uint16_t>(v.count() - 1) : 0;
            Frame frame{cur, idx, v.count()};
            if (v.count() > 0 && v.isFixedRunAt(idx)) {
                std::uint32_t runCount = v.fixedRunCountAt(idx);
                frame.runIndex = runCount > 0 ? runCount - 1 : 0;
            } else if (v.count() > 0 && v.isSubTreeAt(idx)) {
                // Needs the size up front (unlike first()/seek(), which can
                // rely on lazy caching since they always want index 0) --
                // materialize eagerly here.
                auto vals = tree_.collectSubTreeValues(v.subTreeRootAt(idx));
                if (!vals) return vals.status();
                auto ptr = std::make_shared<std::vector<ByteBuffer>>(vals.moveValue());
                frame.runIndex = ptr->empty() ? 0 : static_cast<std::uint32_t>(ptr->size() - 1);
                frame.subTreeValues = std::move(ptr);
            }
            stack_.push_back(std::move(frame));
            return Status::OK();
        }
        if (h->type == PageType::Branch) {
            BranchPageView v(h);
            std::uint16_t idx = static_cast<std::uint16_t>(v.count() - 1);
            stack_.push_back(Frame{cur, idx, v.count()});
            cur = v.childAt(idx);
            continue;
        }
        return Status::Corruption("unexpected page type during cursor descent");
    }
}

Status Cursor::descendAppendToKey(PageId pageId, ByteView key) {
    PageId cur = pageId;
    BoundedWalk depth(kMaxTreeDepth);
    while (true) {
        Status ds = depth.step("cursor descend seek");
        if (!ds.ok()) return ds;
        auto pr = pageManager().getPage(cur);
        if (!pr) return pr.status();
        const PageHeader* h = pr.value();
        if (h->type == PageType::Leaf) {
            LeafPageView v(h);
            std::uint16_t idx = v.lowerBound(key, order_);
            stack_.push_back(Frame{cur, idx, v.count()});
            return Status::OK();
        }
        if (h->type == PageType::Branch) {
            BranchPageView v(h);
            std::uint16_t idx = v.findChildIndex(key, order_);
            stack_.push_back(Frame{cur, idx, v.count()});
            cur = v.childAt(idx);
            continue;
        }
        return Status::Corruption("unexpected page type during cursor descent");
    }
}

Result<bool> Cursor::ascendAndAdvance() {
    if (stack_.empty()) return Result<bool>(false);
    stack_.pop_back();
    while (!stack_.empty()) {
        Frame& parent = stack_.back();
        if (static_cast<std::uint16_t>(parent.index + 1) < parent.count) {
            parent.index = static_cast<std::uint16_t>(parent.index + 1);
            auto pr = pageManager().getPage(parent.pageId);
            if (!pr) return pr.status();
            BranchPageView v(pr.value());
            PageId childId = v.childAt(parent.index);
            Status s = descendAppendFirst(childId);
            if (!s.ok()) return s;
            return Result<bool>(true);
        }
        stack_.pop_back();
    }
    return Result<bool>(false);
}

Result<bool> Cursor::ascendAndRetreat() {
    if (stack_.empty()) return Result<bool>(false);
    stack_.pop_back();
    while (!stack_.empty()) {
        Frame& parent = stack_.back();
        if (parent.index > 0) {
            parent.index = static_cast<std::uint16_t>(parent.index - 1);
            auto pr = pageManager().getPage(parent.pageId);
            if (!pr) return pr.status();
            BranchPageView v(pr.value());
            PageId childId = v.childAt(parent.index);
            Status s = descendAppendLast(childId);
            if (!s.ok()) return s;
            return Result<bool>(true);
        }
        stack_.pop_back();
    }
    return Result<bool>(false);
}

Result<bool> Cursor::first() {
    Status gs = checkGeneration();
    if (!gs.ok()) return gs;
    stack_.clear();
    PageId root = rootAccessor_();
    if (root == kInvalidPageId) return Result<bool>(false);
    Status s = descendAppendFirst(root);
    if (!s.ok()) return s;
    if (stack_.back().count == 0) {  // defensive: should not occur (see remove() invariants)
        stack_.clear();
        return Result<bool>(false);
    }
    return Result<bool>(true);
}

Result<bool> Cursor::last() {
    Status gs = checkGeneration();
    if (!gs.ok()) return gs;
    stack_.clear();
    PageId root = rootAccessor_();
    if (root == kInvalidPageId) return Result<bool>(false);
    Status s = descendAppendLast(root);
    if (!s.ok()) return s;
    if (stack_.back().count == 0) {
        stack_.clear();
        return Result<bool>(false);
    }
    return Result<bool>(true);
}

Result<bool> Cursor::seek(ByteView key) {
    Status gs = checkGeneration();
    if (!gs.ok()) return gs;
    stack_.clear();
    PageId root = rootAccessor_();
    if (root == kInvalidPageId) return Result<bool>(false);
    Status s = descendAppendToKey(root, key);
    if (!s.ok()) return s;
    if (stack_.back().index >= stack_.back().count) return ascendAndAdvance();
    return Result<bool>(true);
}

Result<bool> Cursor::seekExact(ByteView key) {
    auto r = seek(key);
    if (!r) return r.status();
    if (!r.value()) return Status::NotFound();
    auto k = this->key();
    if (!k) return k.status();
    if (order_.cmpKey(k.value(), key) != 0) {
        stack_.clear();
        return Status::NotFound();
    }
    return Result<bool>(true);
}

Result<bool> Cursor::next() {
    Status gs = checkGeneration();
    if (!gs.ok()) return gs;
    if (stack_.empty()) return Result<bool>(false);
    Frame& top = stack_.back();
    // Fixed-run dup cell / DUPSORT escalation sub-tree: step within the
    // group before advancing the slot.
    if (isCurrentFixedRun()) {
        auto pr = pageManager().getPage(top.pageId);
        if (!pr) return pr.status();
        LeafPageView v(pr.value());
        std::uint32_t runCount = v.fixedRunCountAt(top.index);
        if (top.runIndex + 1 < runCount) {
            ++top.runIndex;
            return Result<bool>(true);
        }
        // Run exhausted -- fall through to the normal slot advance below.
    } else if (isCurrentSubTree()) {
        Status es = ensureSubTreeValuesCached();
        if (!es.ok()) return es;
        std::size_t sz = top.subTreeValues ? top.subTreeValues->size() : 0;
        if (static_cast<std::size_t>(top.runIndex) + 1 < sz) {
            ++top.runIndex;
            return Result<bool>(true);
        }
        // Sub-tree exhausted -- fall through to the normal slot advance below.
    }
    if (static_cast<std::uint16_t>(top.index + 1) < top.count) {
        top.index = static_cast<std::uint16_t>(top.index + 1);
        top.runIndex = 0;
        top.subTreeValues.reset();
        return Result<bool>(true);
    }
    return ascendAndAdvance();
}

Result<bool> Cursor::prev() {
    Status gs = checkGeneration();
    if (!gs.ok()) return gs;
    if (stack_.empty()) return Result<bool>(false);
    Frame& top = stack_.back();
    if (isCurrentFixedRun() && top.runIndex > 0) {
        --top.runIndex;
        return Result<bool>(true);
    }
    if (isCurrentSubTree()) {
        Status es = ensureSubTreeValuesCached();
        if (!es.ok()) return es;
        if (top.runIndex > 0) {
            --top.runIndex;
            return Result<bool>(true);
        }
    }
    if (top.index > 0) {
        top.index = static_cast<std::uint16_t>(top.index - 1);
        top.subTreeValues.reset();
        // Moving to a different (possibly fixed-run/sub-tree) cell within
        // the same leaf: land on its LAST element, mirroring descendAppendLast.
        auto pr = pageManager().getPage(top.pageId);
        if (pr) {
            LeafPageView v(pr.value());
            if (top.index < v.count() && v.isFixedRunAt(top.index)) {
                std::uint32_t c = v.fixedRunCountAt(top.index);
                top.runIndex = c > 0 ? c - 1 : 0;
            } else if (top.index < v.count() && v.isSubTreeAt(top.index)) {
                auto vals = tree_.collectSubTreeValues(v.subTreeRootAt(top.index));
                if (!vals) return vals.status();
                auto ptr = std::make_shared<std::vector<ByteBuffer>>(vals.moveValue());
                top.runIndex = ptr->empty() ? 0 : static_cast<std::uint32_t>(ptr->size() - 1);
                top.subTreeValues = std::move(ptr);
            } else {
                top.runIndex = 0;
            }
        } else {
            top.runIndex = 0;
        }
        return Result<bool>(true);
    }
    return ascendAndRetreat();
}

Result<ByteView> Cursor::key() const {
    if (stack_.empty()) return Status::InvalidArgument("cursor is not positioned on a valid entry");
    const Frame& top = stack_.back();
    auto pr = pageManager().getPage(top.pageId);
    if (!pr) return pr.status();
    LeafPageView v(pr.value());
    return Result<ByteView>(v.keyAt(top.index));
}

bool Cursor::isOverflowValue() const {
    if (stack_.empty()) return false;
    const Frame& top = stack_.back();
    auto pr = pageManager().getPage(top.pageId);
    if (!pr) return false;
    LeafPageView v(pr.value());
    if (top.index >= v.count()) return false;
    return v.isOverflowAt(top.index);
}

Result<ByteView> Cursor::valueView() const {
    if (stack_.empty()) return Status::InvalidArgument("cursor is not positioned on a valid entry");
    const Frame& top = stack_.back();
    auto pr = pageManager().getPage(top.pageId);
    if (!pr) return pr.status();
    LeafPageView v(pr.value());
    if (top.index >= v.count()) return Status::InvalidArgument("cursor index out of range");
    if (v.isOverflowAt(top.index)) {
        return Status::InvalidArgument("value is overflow-chained; use value() to materialize");
    }
    if (v.isFixedRunAt(top.index)) {
        ByteView run = v.fixedRunBytesAt(top.index);
        std::uint32_t elemSize = v.fixedRunElemSizeAt(top.index);
        std::size_t off = static_cast<std::size_t>(top.runIndex) * elemSize;
        if (elemSize == 0 || off + elemSize > run.size()) {
            return Status::Corruption("cursor fixed-run position out of range");
        }
        return Result<ByteView>(ByteView(run.data() + off, elemSize));
    }
    if (v.isSubTreeAt(top.index)) {
        Status es = ensureSubTreeValuesCached();
        if (!es.ok()) return es;
        if (!top.subTreeValues || top.runIndex >= top.subTreeValues->size()) {
            return Status::Corruption("cursor sub-tree position out of range");
        }
        const ByteBuffer& elem = (*top.subTreeValues)[top.runIndex];
        return Result<ByteView>(ByteView(elem.data(), elem.size()));
    }
    if (v.isCompressedAt(top.index)) {
        return Status::InvalidArgument("value is compressed; use value() to materialize");
    }
    return Result<ByteView>(v.inlineValueAt(top.index));
}

Result<ByteBuffer> Cursor::value() const {
    if (stack_.empty()) return Status::InvalidArgument("cursor is not positioned on a valid entry");
    const Frame& top = stack_.back();
    auto pr = pageManager().getPage(top.pageId);
    if (!pr) return pr.status();
    LeafPageView v(pr.value());
    if (top.index < v.count() && (v.isFixedRunAt(top.index) || v.isSubTreeAt(top.index))) {
        // One element, not the whole packed blob/sub-tree -- mirrors valueView().
        auto vv = valueView();
        if (!vv) return vv.status();
        return Result<ByteBuffer>(toByteBuffer(vv.value()));
    }
    LeafEntry entry = v.entryAt(top.index);
    return tree_.materializeValue(entry);
}


Result<bool> Cursor::seekKeyValue(ByteView key, ByteView value) {
    auto r = seek(key);
    if (!r) return r.status();
    if (!r.value()) return Status::NotFound();
    // Walk forward while key matches looking for value.
    for (;;) {
        auto k = this->key();
        if (!k) return k.status();
        if (order_.cmpKey(k.value(), key) != 0) {
            stack_.clear();
            return Status::NotFound();
        }
        if (isOverflowValue()) {
            auto v = this->value();
            if (!v) return v.status();
            if (order_.cmpValue(toByteView(v.value()), value) == 0) return Result<bool>(true);
        } else {
            auto v = valueView();
            if (!v) return v.status();
            if (order_.cmpValue(v.value(), value) == 0) return Result<bool>(true);
        }
        auto n = next();
        if (!n) return n.status();
        if (!n.value()) {
            stack_.clear();
            return Status::NotFound();
        }
    }
}

Result<bool> Cursor::firstDup() {
    if (stack_.empty()) return Status::InvalidArgument("cursor not positioned");
    auto k = currentKeyOwned();
    if (!k) return k.status();
    return seekExact(toByteView(k.value()));
}

Result<bool> Cursor::lastDup() {
    if (stack_.empty()) return Status::InvalidArgument("cursor not positioned");
    auto kBuf = currentKeyOwned();
    if (!kBuf) return kBuf.status();
    ByteView key = toByteView(kBuf.value());
    // Seek first of key, then walk to last dup.
    auto r = seekExact(key);
    if (!r) return r.status();
    if (!r.value()) return Status::NotFound();
    for (;;) {
        auto n = next();
        if (!n) return n.status();
        if (!n.value()) {
            // fell off end — step back to last entry
            auto p = prev();
            if (!p) return p.status();
            return Result<bool>(p.value());
        }
        auto k = this->key();
        if (!k) return k.status();
        if (order_.cmpKey(k.value(), key) != 0) {
            auto p = prev();
            if (!p) return p.status();
            return Result<bool>(true);
        }
    }
}

Result<bool> Cursor::nextDup() {
    if (stack_.empty()) return Result<bool>(false);
    auto kBuf = currentKeyOwned();
    if (!kBuf) return kBuf.status();
    ByteView key = toByteView(kBuf.value());
    std::vector<Frame> saved = stack_;
    auto n = next();
    if (!n) return n.status();
    if (n.value()) {
        auto k = this->key();
        if (!k) return k.status();
        if (order_.cmpKey(k.value(), key) == 0) return Result<bool>(true);
    }
    // No further dup of this key: restore the original position (LMDB
    // MDB_NEXT_DUP leaves the cursor unchanged on NotFound) rather than
    // leaving the caller stranded on the next key.
    stack_ = std::move(saved);
    return Result<bool>(false);
}

Result<bool> Cursor::prevDup() {
    if (stack_.empty()) return Result<bool>(false);
    auto kBuf = currentKeyOwned();
    if (!kBuf) return kBuf.status();
    ByteView key = toByteView(kBuf.value());
    std::vector<Frame> saved = stack_;
    auto n = prev();
    if (!n) return n.status();
    if (n.value()) {
        auto k = this->key();
        if (!k) return k.status();
        if (order_.cmpKey(k.value(), key) == 0) return Result<bool>(true);
    }
    // Mirror nextDup(): leave the cursor positioned where it was rather than
    // stranded on the previous key when there's no earlier dup.
    stack_ = std::move(saved);
    return Result<bool>(false);
}

Result<bool> Cursor::nextNoDup() {
    if (stack_.empty()) return Result<bool>(false);
    auto kBuf = currentKeyOwned();
    if (!kBuf) return kBuf.status();
    ByteView key = toByteView(kBuf.value());
    for (;;) {
        auto n = next();
        if (!n) return n.status();
        if (!n.value()) return Result<bool>(false);
        auto k = this->key();
        if (!k) return k.status();
        if (order_.cmpKey(k.value(), key) != 0) return Result<bool>(true);
    }
}

Result<bool> Cursor::prevNoDup() {
    if (stack_.empty()) return Result<bool>(false);
    auto kBuf = currentKeyOwned();
    if (!kBuf) return kBuf.status();
    ByteView key = toByteView(kBuf.value());
    // Move to first of current key, then prev.
    auto fd = firstDup();
    if (!fd) return fd.status();
    return prev();
}

Result<ByteBuffer> Cursor::currentKeyOwned() const {
    auto k = key();
    if (!k) return k.status();
    return Result<ByteBuffer>(toByteBuffer(k.value()));
}

Result<ByteBuffer> Cursor::currentValueOwned() const {
    return value();
}

Status Cursor::del() {
    if (!isWriteBound()) return Status::InvalidArgument("cursor is not write-bound");
    Status gs = checkGeneration();
    if (!gs.ok()) return gs;
    if (stack_.empty()) return Status::InvalidArgument("cursor not positioned");
    if (isCurrentFixedRun()) {
        // Deleting a single element from a DUPFIXED fixed-run cell isn't
        // supported (BTree::removeOne rejects it too) -- explicitly refuse
        // rather than silently falling back to deleting the *whole* run,
        // which is what the generic fallback below would otherwise do.
        return Status::InvalidArgument(
            "del() cannot remove a single element from a DUPFIXED fixed-run; "
            "remove() the whole key instead");
    }

    auto kBuf = currentKeyOwned();
    if (!kBuf) return kBuf.status();
    auto vBuf = currentValueOwned();
    if (!vBuf) return vBuf.status();

    // Prefer removeOne so only this dup is deleted under DUPSORT.
    auto r = writeTree_->removeOne(*writeRoot_, writeTxnId_, toByteView(kBuf.value()),
                                   toByteView(vBuf.value()), flags_, &order_);
    if (!r) {
        // Unique tree / overflow: fall back to removing by key only when single match.
        auto r2 = writeTree_->remove(*writeRoot_, writeTxnId_, toByteView(kBuf.value()), flags_, &order_);
        if (!r2) return r2.status();
        *writeRoot_ = r2.value();
    } else {
        *writeRoot_ = r.value();
    }

    // Reposition: seek to first >= deleted key; if same key+value still there (shouldn't), next.
    ByteBuffer keyCopy = kBuf.moveValue();
    ByteBuffer valCopy = vBuf.moveValue();
    stack_.clear();
    auto s = seek(toByteView(keyCopy));
    if (!s.ok()) return s.status();
    if (!s.value()) return Status::OK();  // empty after delete
    // If we landed on the same key with a value that sorts at/after deleted, OK.
    // If exact same pair somehow remains, step next.
    auto k = key();
    if (k.ok() && order_.cmpKey(k.value(), toByteView(keyCopy)) == 0) {
        if (!isOverflowValue()) {
            auto vv = valueView();
            if (vv.ok() && order_.cmpValue(vv.value(), toByteView(valCopy)) == 0) {
                auto n = next();
                (void)n;
            }
        }
    }
    return Status::OK();
}

Status Cursor::put(ByteView key, ByteView value, CursorPutFlags flags) {
    if (!isWriteBound()) return Status::InvalidArgument("cursor is not write-bound");
    Status gs = checkGeneration();
    if (!gs.ok()) return gs;

    if (hasFlag(flags, CursorPutFlags::Append)) {
        auto ins = writeTree_->insertAppend(*writeRoot_, writeTxnId_, key, value, flags_);
        if (!ins) return ins.status();
        *writeRoot_ = ins.value();
        stack_.clear();
        auto r = seekExact(key);
        if (!r) return r.status();
        if (!r.value()) return Status::Internal("cursor put reposition failed");
        return Status::OK();
    }

    ByteBuffer keyOwned;
    if (hasFlag(flags, CursorPutFlags::Current)) {
        if (stack_.empty()) return Status::InvalidArgument("MDB_CURRENT requires positioned cursor");
        auto k = currentKeyOwned();
        if (!k) return k.status();
        keyOwned = k.moveValue();
        if (!key.empty() && compareBytes(key, toByteView(keyOwned)) != 0) {
            return Status::InvalidArgument("MDB_CURRENT key mismatch");
        }
        key = toByteView(keyOwned);
        // Replace: unique overwrite of this key's first value, or removeOne+putDup for dups.
        // Simplest: removeOne current value then putDup/put.
        auto oldV = currentValueOwned();
        if (!oldV) return oldV.status();
        if (hasFlag(flags, CursorPutFlags::AppendDup)) {
            auto rm = writeTree_->removeOne(*writeRoot_, writeTxnId_, key, toByteView(oldV.value()),
                                            DbFlags::None, &order_);
            if (!rm) return rm.status();
            *writeRoot_ = rm.value();
            auto ins = writeTree_->insert(*writeRoot_, writeTxnId_, key, value,
                                          /*overwrite=*/true, /*allowDups=*/true, flags_, &order_);
            if (!ins) return ins.status();
            *writeRoot_ = ins.value();
        } else {
            auto ins = writeTree_->insert(*writeRoot_, writeTxnId_, key, value,
                                          /*overwrite=*/true, /*allowDups=*/false, flags_, &order_);
            if (!ins) return ins.status();
            *writeRoot_ = ins.value();
        }
        stack_.clear();
        {
            auto r = seekKeyValue(key, value);
            if (r.ok() && r.value()) return Status::OK();
            auto r2 = seekExact(key);
            if (r2.ok() && r2.value()) return Status::OK();
            return Status::Internal("cursor put CURRENT reposition failed");
        }
    }

    const bool allowDups = hasFlag(flags, CursorPutFlags::AppendDup);
    const bool overwrite = !hasFlag(flags, CursorPutFlags::NoOverwrite);
    // noDupData when AppendDup && NoOverwrite → overwrite=false
    auto ins = writeTree_->insert(*writeRoot_, writeTxnId_, key, value, overwrite, allowDups, flags_, &order_);
    if (!ins) return ins.status();
    *writeRoot_ = ins.value();
    stack_.clear();
    if (allowDups) {
        auto r = seekKeyValue(key, value);
        if (!r) return r.status();
        if (!r.value()) return Status::Internal("cursor put reposition failed");
    } else {
        auto r = seekExact(key);
        if (!r) return r.status();
        if (!r.value()) return Status::Internal("cursor put reposition failed");
    }
    return Status::OK();
}

Status Cursor::putMultiple(ByteView key, std::uint32_t elemSize, ByteView valuesBlob) {
    if (!isWriteBound()) return Status::InvalidArgument("cursor is not write-bound");
    Status gs = checkGeneration();
    if (!gs.ok()) return gs;

    auto ins = writeTree_->insertMultiple(*writeRoot_, writeTxnId_, key, elemSize, valuesBlob, flags_);
    if (!ins) return ins.status();
    *writeRoot_ = ins.value();
    stack_.clear();
    auto r = seekExact(key);
    if (!r) return r.status();
    if (!r.value()) return Status::Internal("cursor putMultiple reposition failed");
    return Status::OK();
}

Result<std::size_t> Cursor::count() const {
    if (stack_.empty()) return Status::InvalidArgument("cursor not positioned");
    const Frame& top = stack_.back();
    auto pr = pageManager().getPage(top.pageId);
    if (!pr) return pr.status();
    LeafPageView v(pr.value());
    if (top.index >= v.count()) return Status::InvalidArgument("cursor index out of range");
    if (v.isFixedRunAt(top.index)) {
        return Result<std::size_t>(static_cast<std::size_t>(v.fixedRunCountAt(top.index)));
    }
    if (v.isSubTreeAt(top.index)) {
        auto c = tree_.countSubTreeValues(v.subTreeRootAt(top.index));
        if (!c) return c.status();
        return Result<std::size_t>(c.value());
    }
    // Flat (unescalated) dup storage: count the contiguous same-key run in
    // *this* leaf around the current position (see the header comment for
    // the leaf-boundary caveat this shares with the rest of flat storage).
    ByteView curKey = v.keyAt(top.index);
    std::uint16_t i = top.index;
    while (i > 0 && order_.cmpKey(v.keyAt(static_cast<std::uint16_t>(i - 1)), curKey) == 0) --i;
    std::uint16_t n = 0;
    while (static_cast<std::uint16_t>(i + n) < v.count() &&
           order_.cmpKey(v.keyAt(static_cast<std::uint16_t>(i + n)), curKey) == 0) {
        ++n;
    }
    return Result<std::size_t>(static_cast<std::size_t>(n));
}

Result<std::optional<ByteBuffer>> Cursor::replace(ByteView key, ByteView value) {
    if (!isWriteBound()) return Status::InvalidArgument("cursor is not write-bound");
    // seekExact's contract: an error Status::NotFound when the key is
    // absent, not an ok-but-false result (unlike seek()/next()/prev()) --
    // that's the expected, non-fatal "key doesn't exist yet" case here, not
    // a real error to propagate.
    auto seekR = seekExact(key);
    std::optional<ByteBuffer> oldValue;
    if (seekR.ok()) {
        auto oldR = this->value();
        if (!oldR) return oldR.status();
        oldValue = oldR.moveValue();
    } else if (seekR.status().code() != StatusCode::NotFound) {
        return seekR.status();
    }
    Status putS = put(key, value, CursorPutFlags::None);
    if (!putS.ok()) return putS;
    return Result<std::optional<ByteBuffer>>(std::move(oldValue));
}

Result<std::optional<ByteBuffer>> Cursor::pop(ByteView key) {
    if (!isWriteBound()) return Status::InvalidArgument("cursor is not write-bound");
    auto seekR = seekExact(key);
    if (!seekR) {
        if (seekR.status().code() == StatusCode::NotFound) {
            return Result<std::optional<ByteBuffer>>(std::nullopt);
        }
        return seekR.status();
    }
    auto oldR = this->value();
    if (!oldR) return oldR.status();
    ByteBuffer oldValue = oldR.moveValue();
    Status delS = del();
    if (!delS.ok()) return delS;
    return Result<std::optional<ByteBuffer>>(std::move(oldValue));
}

Result<std::vector<std::pair<ByteBuffer, ByteBuffer>>> Cursor::getMulti(const std::vector<ByteView>& keys,
                                                                        bool dupData) {
    std::vector<std::pair<ByteBuffer, ByteBuffer>> results;
    for (ByteView k : keys) {
        auto r = seekExact(k);
        if (!r) {
            if (r.status().code() == StatusCode::NotFound) continue;  // missing keys silently skipped
            return r.status();
        }
        if (!dupData) {
            auto v = value();
            if (!v) return v.status();
            results.emplace_back(toByteBuffer(k), v.moveValue());
            continue;
        }
        auto first = firstDup();
        if (!first) return first.status();
        if (!first.value()) continue;
        for (;;) {
            auto v = value();
            if (!v) return v.status();
            results.emplace_back(toByteBuffer(k), v.moveValue());
            auto n = nextDup();
            if (!n) return n.status();
            if (!n.value()) break;
        }
    }
    return Result<std::vector<std::pair<ByteBuffer, ByteBuffer>>>(std::move(results));
}

Result<Cursor::PutMultiResult> Cursor::putMulti(const std::vector<std::pair<ByteView, ByteView>>& items,
                                                bool overwrite, bool append) {
    if (!isWriteBound()) return Status::InvalidArgument("cursor is not write-bound");
    PutMultiResult result;
    CursorPutFlags baseFlags = overwrite ? CursorPutFlags::None : CursorPutFlags::NoOverwrite;
    if (append) baseFlags = baseFlags | CursorPutFlags::Append;
    for (const auto& item : items) {
        ++result.consumed;
        Status s = put(item.first, item.second, baseFlags);
        if (s.ok()) {
            ++result.added;
        } else if (s.code() == StatusCode::KeyExists) {
            continue;  // overwrite=false and key already present -- not fatal, just not "added"
        } else {
            return s;
        }
    }
    return Result<PutMultiResult>(result);
}

Result<bool> Cursor::seekRangeDup(ByteView key, ByteView value) {
    auto r = seekExact(key);
    if (!r) return r.status();
    if (!r.value()) return Status::NotFound();
    auto first = firstDup();
    if (!first) return first.status();
    if (!first.value()) return Result<bool>(false);
    for (;;) {
        auto k = this->key();
        if (!k) return k.status();
        if (order_.cmpKey(k.value(), key) != 0) {
            stack_.clear();
            return Status::NotFound();
        }
        ByteBuffer ownedVal;
        ByteView curVal;
        if (isOverflowValue()) {
            auto vv = this->value();
            if (!vv) return vv.status();
            ownedVal = vv.moveValue();
            curVal = toByteView(ownedVal);
        } else {
            auto vv = valueView();
            if (!vv) return vv.status();
            curVal = vv.value();
        }
        if (order_.cmpValue(curVal, value) >= 0) return Result<bool>(true);
        auto n = nextDup();
        if (!n) return n.status();
        if (!n.value()) return Result<bool>(false);  // exhausted this key's dups, none >= value
    }
}

}  // namespace annpocketdb::btree
