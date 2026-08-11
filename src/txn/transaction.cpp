#include "annpocketdb/txn/transaction.hpp"
#include "annpocketdb/changefeed/changefeed.hpp"

namespace annpocketdb::txn {

// ---------------------------------------------------------------------------
// ReadTransaction
// ---------------------------------------------------------------------------

ReadTransaction::ReadTransaction(core::PageManager& pm, btree::BTree& tree, core::Snapshot snapshot,
                                  core::ReaderHandle handle)
    : pm_(&pm), tree_(&tree), snapshot_(snapshot), handle_(handle) {}

ReadTransaction::~ReadTransaction() {
    if (pm_ != nullptr && handle_ != core::kInvalidReaderHandle) pm_->unregisterReader(handle_);
}

ReadTransaction::ReadTransaction(ReadTransaction&& other) noexcept
    : pm_(other.pm_), tree_(other.tree_), snapshot_(other.snapshot_), handle_(other.handle_),
      subDbCatalog_(other.subDbCatalog_), subDbs_(std::move(other.subDbs_)),
      reboundFlags_(other.reboundFlags_), reboundKeyCmp_(std::move(other.reboundKeyCmp_)),
      reboundValueCmp_(std::move(other.reboundValueCmp_)) {
    other.pm_ = nullptr;
    other.handle_ = core::kInvalidReaderHandle;
    other.subDbCatalog_ = nullptr;
}

ReadTransaction& ReadTransaction::operator=(ReadTransaction&& other) noexcept {
    if (this == &other) return *this;
    if (pm_ != nullptr && handle_ != core::kInvalidReaderHandle) pm_->unregisterReader(handle_);
    pm_ = other.pm_;
    tree_ = other.tree_;
    snapshot_ = other.snapshot_;
    handle_ = other.handle_;
    subDbCatalog_ = other.subDbCatalog_;
    subDbs_ = std::move(other.subDbs_);
    reboundFlags_ = other.reboundFlags_;
    reboundKeyCmp_ = std::move(other.reboundKeyCmp_);
    reboundValueCmp_ = std::move(other.reboundValueCmp_);
    other.pm_ = nullptr;
    other.handle_ = core::kInvalidReaderHandle;
    other.subDbCatalog_ = nullptr;
    return *this;
}

Result<ByteBuffer> ReadTransaction::get(ByteView key) const {
    btree::KeyOrder order = reboundOrder();
    return tree_->get(snapshot_.masterRootPageId, key, reboundFlags_, &order);
}

Result<ByteView> ReadTransaction::getView(ByteView key) const {
    btree::KeyOrder order = reboundOrder();
    return tree_->getView(snapshot_.masterRootPageId, key, reboundFlags_, &order);
}

Result<bool> ReadTransaction::contains(ByteView key) const {
    btree::KeyOrder order = reboundOrder();
    return tree_->contains(snapshot_.masterRootPageId, key, reboundFlags_, &order);
}

Result<std::vector<ByteBuffer>> ReadTransaction::getAll(ByteView key) const {
    btree::KeyOrder order = reboundOrder();
    return tree_->getAll(snapshot_.masterRootPageId, key, reboundFlags_, &order);
}

btree::Cursor ReadTransaction::openCursor() const {
    PageId root = snapshot_.masterRootPageId;
    btree::Cursor c(*tree_, [root]() { return root; });
    c.setFlags(reboundFlags_);
    c.setCustomComparators(reboundKeyCmp_, reboundValueCmp_);
    return c;
}

Status ReadTransaction::reset() {
    if (pm_ != nullptr && handle_ != core::kInvalidReaderHandle) {
        pm_->unregisterReader(handle_);
    }
    handle_ = core::kInvalidReaderHandle;
    return Status::OK();
}

Status ReadTransaction::renew() {
    if (pm_ == nullptr) return Status::Internal("read transaction has no page manager (moved-from?)");
    if (handle_ != core::kInvalidReaderHandle) {
        return Status::Internal("read transaction already active; call reset() first");
    }
    core::Snapshot snap = pm_->currentSnapshot();
    core::ReaderHandle handle = pm_->registerReader(snap.txnId);
    if (handle == core::kInvalidReaderHandle) {
        return Status::ReadersFull("reader table full; too many concurrent readers across processes");
    }
    snapshot_ = snap;
    handle_ = handle;
    return Status::OK();
}

Result<PageId> ReadTransaction::useSubDb(SubDbHandle h, std::uint32_t openSeq) const {
    auto it = subDbs_.find(h);
    if (it != subDbs_.end()) {
        if (!subDbCatalog_ || !subDbCatalog_->sequenceMatches(h, it->second.seq)) {
            subDbs_.erase(it);
            return Status::BadDbi("stale sub-database handle (changed during transaction)");
        }
        return Result<PageId>(it->second.root);
    }
    if (!subDbCatalog_) return Status::Internal("no sub-database catalog attached");
    auto keyR = subDbCatalog_->validateHandle(h, openSeq);
    if (!keyR) return keyR.status();
    auto got = get(toByteView(keyR.value()));
    if (!got) return got.status();
    PageId root;
    Status ds = subDbCatalog_->decodeCatalogRoot(toByteView(got.value()), root);
    if (!ds.ok()) return ds;
    SubDbTxnEntry entry;
    entry.root = root;
    entry.seq = openSeq;
    entry.catalogKey = keyR.value();
    subDbs_[h] = std::move(entry);
    return Result<PageId>(root);
}

Status ReadTransaction::rebindToSubDb(SubDbHandle h, std::uint32_t openSeq) {
    auto r = useSubDb(h, openSeq);
    if (!r) return r.status();
    snapshot_.masterRootPageId = r.value();
    if (subDbCatalog_) {
        reboundFlags_ = subDbCatalog_->flagsOf(h);
        btree::KeyOrder order = subDbCatalog_->keyOrderOf(h);
        reboundKeyCmp_ = order.customKeyCmp;
        reboundValueCmp_ = order.customValueCmp;
    }
    return Status::OK();
}

Result<ByteBuffer> ReadTransaction::getSubDb(SubDbHandle h, std::uint32_t openSeq, ByteView key) const {
    auto r = useSubDb(h, openSeq);
    if (!r) return r.status();
    btree::KeyOrder order = subDbCatalog_->keyOrderOf(h);
    return tree_->get(r.value(), key, subDbCatalog_->flagsOf(h), &order);
}

Result<bool> ReadTransaction::containsSubDb(SubDbHandle h, std::uint32_t openSeq, ByteView key) const {
    auto r = useSubDb(h, openSeq);
    if (!r) return r.status();
    btree::KeyOrder order = subDbCatalog_->keyOrderOf(h);
    return tree_->contains(r.value(), key, subDbCatalog_->flagsOf(h), &order);
}

btree::Cursor ReadTransaction::openCursorSubDb(SubDbHandle h, std::uint32_t openSeq) const {
    auto r = useSubDb(h, openSeq);
    if (!r) return btree::Cursor(*tree_, []() { return kInvalidPageId; });
    PageId root = r.value();
    btree::Cursor c(*tree_, [root]() { return root; });
    if (subDbCatalog_) {
        c.setFlags(subDbCatalog_->flagsOf(h));
        btree::KeyOrder order = subDbCatalog_->keyOrderOf(h);
        c.setCustomComparators(order.customKeyCmp, order.customValueCmp);
    }
    return c;
}

// ---------------------------------------------------------------------------
// WriteTransaction
// ---------------------------------------------------------------------------

WriteTransaction::WriteTransaction(core::PageManager& pm, btree::BTree& tree)
    : pm_(&pm), tree_(&tree), lock_(pm.writerMutex()), finished_(false) {
    pendingError_ = pm.refreshFromDiskIfStale();
    core::Snapshot snap = pm.currentSnapshot();
    txnId_ = snap.txnId + 1;
    masterRootPageId_ = snap.masterRootPageId;
    walSequence_ = snap.walSequence;
}

WriteTransaction::~WriteTransaction() {
    if (!finished_) (void)abort();
}

WriteTransaction::WriteTransaction(WriteTransaction&& other) noexcept
    : pm_(other.pm_),
      tree_(other.tree_),
      lock_(std::move(other.lock_)),
      txnId_(other.txnId_),
      masterRootPageId_(other.masterRootPageId_),
      walSequence_(other.walSequence_),
      finished_(other.finished_),
      pendingError_(other.pendingError_),
      generation_(other.generation_),
      changeSink_(other.changeSink_),
      subDbCatalog_(other.subDbCatalog_),
      subDbs_(std::move(other.subDbs_)),
      prepared_(other.prepared_),
      preparedHandle_(other.preparedHandle_) {
    other.changeSink_ = nullptr;
    other.subDbCatalog_ = nullptr;
    other.prepared_ = false;
    // Prevent the moved-from object's destructor from also trying to
    // abort()/unlock an already-transferred (now non-owning) lock.
    // Zero generation on the moved-from object so any cursor that still
    // holds a pointer to `other` can detect the invalidation.
    other.finished_ = true;
    other.pm_ = nullptr;
    other.generation_ = 0;
}

WriteTransaction& WriteTransaction::operator=(WriteTransaction&& other) noexcept {
    if (this == &other) return *this;
    if (!finished_) (void)abort();
    generation_ = 0;  // invalidate cursors that pointed at the old `this`
    pm_ = other.pm_;
    tree_ = other.tree_;
    lock_ = std::move(other.lock_);
    txnId_ = other.txnId_;
    masterRootPageId_ = other.masterRootPageId_;
    walSequence_ = other.walSequence_;
    finished_ = other.finished_;
    pendingError_ = other.pendingError_;
    generation_ = other.generation_;
    changeSink_ = other.changeSink_;
    subDbCatalog_ = other.subDbCatalog_;
    subDbs_ = std::move(other.subDbs_);
    prepared_ = other.prepared_;
    preparedHandle_ = other.preparedHandle_;
    other.changeSink_ = nullptr;
    other.subDbCatalog_ = nullptr;
    other.prepared_ = false;
    other.finished_ = true;
    other.pm_ = nullptr;
    other.generation_ = 0;
    return *this;
}

// ---- Master-root path ----

Status WriteTransaction::put(ByteView key, ByteView value, bool overwrite) {
    return put(masterRootPageId_, key, value, overwrite);
}

Status WriteTransaction::putDup(ByteView key, ByteView value, bool noDupData) {
    return putDup(masterRootPageId_, key, value, noDupData);
}

Status WriteTransaction::remove(ByteView key) {
    return remove(masterRootPageId_, key);
}

Status WriteTransaction::removeDup(ByteView key, ByteView value) {
    return removeDup(masterRootPageId_, key, value);
}

Result<std::vector<ByteBuffer>> WriteTransaction::getAll(ByteView key) const {
    return getAll(masterRootPageId_, key);
}

Result<ByteBuffer> WriteTransaction::get(ByteView key) const {
    return get(masterRootPageId_, key);
}

Result<ByteView> WriteTransaction::getView(ByteView key) const {
    return getView(masterRootPageId_, key);
}

Result<bool> WriteTransaction::contains(ByteView key) const {
    return contains(masterRootPageId_, key);
}

btree::Cursor WriteTransaction::openCursor() {
    // Capture `this` so a re-seek observes the current masterRootPageId_.
    // bindWrite enables cursor del/put against this transaction; the
    // generation accessor lets the cursor detect a move/destroy of `this`
    // or a nested-transaction abort that happened since it was opened.
    btree::Cursor c(*tree_, [this]() { return masterRootPageId_; });
    c.bindWrite(*tree_, masterRootPageId_, txnId_, [this]() { return generation_; });
    return c;
}

Result<std::optional<ByteBuffer>> WriteTransaction::replace(ByteView key, ByteView value) {
    if (finished_) return Status::Internal("transaction already finished");
    btree::Cursor cur = openCursor();
    return cur.replace(key, value);
}

Result<std::optional<ByteBuffer>> WriteTransaction::pop(ByteView key) {
    if (finished_) return Status::Internal("transaction already finished");
    btree::Cursor cur = openCursor();
    return cur.pop(key);
}


// ---- Explicit-root path ----

Status WriteTransaction::put(PageId& rootPageId, ByteView key, ByteView value, bool overwrite) {
    if (finished_) return Status::Internal("transaction already finished");

    // Capture old value for changefeed when mutating the master keyspace.
    ByteBuffer oldVal;
    bool hadOld = false;
    if (changeSink_ != nullptr && &rootPageId == &masterRootPageId_) {
        auto g = tree_->get(rootPageId, key);
        if (g.ok()) {
            oldVal = g.moveValue();
            hadOld = true;
        } else if (g.status().code() != StatusCode::NotFound) {
            return g.status();
        }
    }

    auto r = tree_->insert(rootPageId, txnId_, key, value, overwrite, /*allowDups=*/false);
    if (!r) return r.status();
    rootPageId = r.value();

    if (changeSink_ != nullptr && &rootPageId == &masterRootPageId_) {
        using changefeed::ChangeType;
        ChangeType type = hadOld ? ChangeType::Update : ChangeType::Insert;
        changeSink_->record(type, txnId_, key,
                            ByteView(oldVal.data(), oldVal.size()), value);
    }
    return Status::OK();
}

Status WriteTransaction::putDup(PageId& rootPageId, ByteView key, ByteView value, bool noDupData) {
    if (finished_) return Status::Internal("transaction already finished");
    // overwrite=false + allowDups: insert new pair or KeyExists on exact match when noDupData.
    // overwrite=true + allowDups: replace exact pair if present, else insert.
    const bool overwrite = !noDupData;
    auto r = tree_->insert(rootPageId, txnId_, key, value, overwrite, /*allowDups=*/true,
                           btree::DbFlags::DupSort);
    if (!r) return r.status();
    rootPageId = r.value();
    // Changefeed: treat each dup as an insert (secondary indexes / multi-value).
    if (changeSink_ != nullptr && &rootPageId == &masterRootPageId_) {
        changeSink_->record(changefeed::ChangeType::Insert, txnId_, key, ByteView{}, value);
    }
    return Status::OK();
}

Status WriteTransaction::removeDup(PageId& rootPageId, ByteView key, ByteView value) {
    if (finished_) return Status::Internal("transaction already finished");
    auto r = tree_->removeOne(rootPageId, txnId_, key, value);
    if (!r) return r.status();
    rootPageId = r.value();
    if (changeSink_ != nullptr && &rootPageId == &masterRootPageId_) {
        changeSink_->record(changefeed::ChangeType::Delete, txnId_, key, value, ByteView{});
    }
    return Status::OK();
}

Result<std::vector<ByteBuffer>> WriteTransaction::getAll(PageId rootPageId, ByteView key) const {
    return tree_->getAll(rootPageId, key);
}

Status WriteTransaction::remove(PageId& rootPageId, ByteView key) {
    if (finished_) return Status::Internal("transaction already finished");

    ByteBuffer oldVal;
    if (changeSink_ != nullptr && &rootPageId == &masterRootPageId_) {
        auto g = tree_->get(rootPageId, key);
        if (g.ok()) {
            oldVal = g.moveValue();
        } else if (g.status().code() != StatusCode::NotFound) {
            return g.status();
        } else {
            // Nothing to delete — still attempt remove for consistent errors.
        }
    }

    auto r = tree_->remove(rootPageId, txnId_, key);
    if (!r) return r.status();
    rootPageId = r.value();

    if (changeSink_ != nullptr && &rootPageId == &masterRootPageId_ && !oldVal.empty()) {
        changeSink_->record(changefeed::ChangeType::Delete, txnId_, key,
                            ByteView(oldVal.data(), oldVal.size()), ByteView{});
    }
    return Status::OK();
}

Result<ByteBuffer> WriteTransaction::get(PageId rootPageId, ByteView key) const {
    return tree_->get(rootPageId, key);
}

Result<ByteView> WriteTransaction::getView(PageId rootPageId, ByteView key) const {
    return tree_->getView(rootPageId, key);
}

Result<bool> WriteTransaction::contains(PageId rootPageId, ByteView key) const {
    return tree_->contains(rootPageId, key);
}

btree::Cursor WriteTransaction::openCursor(PageId& rootPageId) {
    // Caller-owned root: cursor mutations update rootPageId in place.
    btree::Cursor c(*tree_, [&rootPageId]() { return rootPageId; });
    c.bindWrite(*tree_, rootPageId, txnId_, [this]() { return generation_; });
    return c;
}

btree::Cursor WriteTransaction::openCursor(PageId rootPageId) const {
    // Read-only view of a fixed root snapshot (no bindWrite).
    return btree::Cursor(*tree_, [rootPageId]() { return rootPageId; });
}

// ---- Named sub-database support ----

Result<PageId> WriteTransaction::useSubDb(SubDbHandle h, std::uint32_t openSeq) const {
    auto it = subDbs_.find(h);
    if (it != subDbs_.end()) {
        if (!subDbCatalog_ || !subDbCatalog_->sequenceMatches(h, it->second.seq)) {
            subDbs_.erase(it);
            return Status::BadDbi("stale sub-database handle (changed during transaction)");
        }
        return Result<PageId>(it->second.root);
    }
    if (!subDbCatalog_) return Status::Internal("no sub-database catalog attached");
    auto keyR = subDbCatalog_->validateHandle(h, openSeq);
    if (!keyR) return keyR.status();
    auto got = get(toByteView(keyR.value()));  // read-your-own-writes: this txn's own view
    if (!got) return got.status();
    PageId root;
    Status ds = subDbCatalog_->decodeCatalogRoot(toByteView(got.value()), root);
    if (!ds.ok()) return ds;
    SubDbTxnEntry entry;
    entry.root = root;
    entry.seq = openSeq;
    entry.catalogKey = keyR.value();
    subDbs_[h] = std::move(entry);
    return Result<PageId>(root);
}

void WriteTransaction::registerNewSubDb(SubDbHandle h, PageId root, std::uint32_t seq, ByteBuffer catalogKey) {
    SubDbTxnEntry entry;
    entry.root = root;
    entry.seq = seq;
    entry.isNew = true;
    entry.catalogKey = std::move(catalogKey);
    subDbs_[h] = std::move(entry);
}

void WriteTransaction::markSubDbDropped(SubDbHandle h) {
    subDbs_[h].dropped = true;
}

Status WriteTransaction::putSubDb(SubDbHandle h, std::uint32_t openSeq, ByteView key, ByteView value,
                                   bool overwrite) {
    auto r = useSubDb(h, openSeq);
    if (!r) return r.status();
    PageId root = r.value();
    // Bypass the flag-blind put(PageId&,...) wrapper (hardcodes
    // allowDups=false, DbFlags::None) -- go straight to BTree so this
    // handle's own persisted flags/custom comparator actually govern
    // storage order, not just Cursor-side navigation.
    btree::DbFlags flags = subDbCatalog_->flagsOf(h);
    btree::KeyOrder order = subDbCatalog_->keyOrderOf(h);
    bool allowDups = btree::hasFlag(flags, btree::DbFlags::DupSort);
    auto ins = tree_->insert(root, txnId_, key, value, overwrite, allowDups, flags, &order);
    if (!ins) return ins.status();
    root = ins.value();
    subDbs_[h].root = root;
    // Publish the updated root into this handle's catalog record now, in
    // the SAME transaction -- this (not the per-txn cache above) is what
    // makes the update visible to any other transaction/handle after commit.
    ByteBuffer encoded;
    Status es = subDbCatalog_->encodeCatalogRoot(h, root, encoded);
    if (!es.ok()) return es;
    return put(toByteView(subDbs_[h].catalogKey), toByteView(encoded), true);
}

Status WriteTransaction::removeSubDb(SubDbHandle h, std::uint32_t openSeq, ByteView key) {
    auto r = useSubDb(h, openSeq);
    if (!r) return r.status();
    PageId root = r.value();
    btree::DbFlags flags = subDbCatalog_->flagsOf(h);
    btree::KeyOrder order = subDbCatalog_->keyOrderOf(h);
    auto rm = tree_->remove(root, txnId_, key, flags, &order);
    if (!rm) return rm.status();
    root = rm.value();
    subDbs_[h].root = root;
    ByteBuffer encoded;
    Status es = subDbCatalog_->encodeCatalogRoot(h, root, encoded);
    if (!es.ok()) return es;
    return put(toByteView(subDbs_[h].catalogKey), toByteView(encoded), true);
}

Result<ByteBuffer> WriteTransaction::getSubDb(SubDbHandle h, std::uint32_t openSeq, ByteView key) const {
    auto r = useSubDb(h, openSeq);
    if (!r) return r.status();
    btree::KeyOrder order = subDbCatalog_->keyOrderOf(h);
    return tree_->get(r.value(), key, subDbCatalog_->flagsOf(h), &order);
}

Result<bool> WriteTransaction::containsSubDb(SubDbHandle h, std::uint32_t openSeq, ByteView key) const {
    auto r = useSubDb(h, openSeq);
    if (!r) return r.status();
    btree::KeyOrder order = subDbCatalog_->keyOrderOf(h);
    return tree_->contains(r.value(), key, subDbCatalog_->flagsOf(h), &order);
}

btree::Cursor WriteTransaction::openCursorSubDb(SubDbHandle h, std::uint32_t openSeq) const {
    auto r = useSubDb(h, openSeq);
    if (!r) return btree::Cursor(*tree_, []() { return kInvalidPageId; });
    btree::Cursor c = openCursor(r.value());
    if (subDbCatalog_) {
        c.setFlags(subDbCatalog_->flagsOf(h));
        btree::KeyOrder order = subDbCatalog_->keyOrderOf(h);
        c.setCustomComparators(order.customKeyCmp, order.customValueCmp);
    }
    return c;
}

PageId WriteTransaction::currentSubDbRoot(SubDbHandle h) const {
    auto it = subDbs_.find(h);
    return it != subDbs_.end() ? it->second.root : kInvalidPageId;
}

Status WriteTransaction::prepare(core::SyncMode mode) {
    if (!pendingError_.ok()) return pendingError_;
    if (finished_) return Status::Internal("transaction already finished");
    if (prepared_) return Status::Internal("transaction already prepared");
    if (!savepoints_.empty()) {
        return Status::InvalidArgument(
            "cannot prepare while nested transactions are open; "
            "commitNested/abortNested first");
    }
    // Durable changefeed: same ordering guarantee as an ordinary commit --
    // logged before the meta write so it's part of what prepare() makes
    // durable; commit() (the finalize step) does not repeat this.
    if (changeSink_ != nullptr) {
        Status fs = changeSink_->flush(*this);
        if (!fs.ok()) return fs;
    }
    auto h = pm_->prepareCommit(txnId_, masterRootPageId_, walSequence_, mode);
    if (!h) return h.status();
    preparedHandle_ = h.value();
    prepared_ = true;
    // Deliberately does not release lock_ or set finished_ -- this
    // transaction stays open, holding the single-writer gate, until the
    // coordinator's decision arrives via commit() or abort().
    return Status::OK();
}

Status WriteTransaction::commit(core::SyncMode mode) {
    if (!pendingError_.ok()) return pendingError_;
    if (finished_) return Status::Internal("transaction already finished");
    if (!savepoints_.empty()) {
        return Status::InvalidArgument(
            "cannot top-level commit while nested transactions are open; "
            "commitNested/abortNested first");
    }

    Status s;
    if (prepared_) {
        // Data + meta already durable from prepare(); this is only the
        // second required durable write (flip Prepared -> Committed on the
        // same slot) plus publish. No changefeed flush here -- already done
        // in prepare().
        s = pm_->finalizePreparedCommit(preparedHandle_, txnId_, masterRootPageId_, walSequence_, mode);
    } else {
        // Durable changefeed: append log entries into the master tree before
        // the meta flip so they become visible exactly when the txn commits.
        if (changeSink_ != nullptr) {
            Status fs = changeSink_->flush(*this);
            if (!fs.ok()) return fs;
        }
        s = pm_->commit(txnId_, masterRootPageId_, walSequence_, mode);
    }
    finished_ = true;
    if (lock_.owns_lock()) lock_.unlock();
    if (s.ok() && changeSink_ != nullptr) {
        changeSink_->notifyCommitted();
    }

    // Sub-database catalog export: only on an actually-durable commit does a
    // dropped handle become invalid for everyone (confirmDrop); a failed
    // commit unwinds any handle this transaction provisionally created
    // (rollbackPendingCreate), exactly mirroring the abort() path below.
    // Ordinary (non-drop, non-new) root updates need no action here -- their
    // catalog record is already durable via the normal commit above.
    if (subDbCatalog_ != nullptr) {
        for (auto& [h, entry] : subDbs_) {
            if (s.ok()) {
                if (entry.dropped) subDbCatalog_->confirmDrop(h);
            } else if (entry.isNew) {
                subDbCatalog_->rollbackPendingCreate(h);
            }
        }
    }
    subDbs_.clear();
    return s;
}

Status WriteTransaction::abort() {
    if (finished_) return Status::OK();
    // Drop any nested frames; restore outermost captured roots if present.
    while (!savepoints_.empty()) {
        Status s = abortNested();
        if (!s.ok()) return s;
    }
    if (prepared_) {
        // Unlike an ordinary abort (which durably wrote nothing), the
        // prepared slot is already on disk and structurally valid -- it
        // must be explicitly invalidated so it can never win a future
        // slot-selection comparison. Leave the transaction open (lock still
        // held, finished_ still false) on failure so the caller can retry.
        Status is = pm_->invalidatePreparedSlot(preparedHandle_, core::SyncMode::Durable);
        if (!is.ok()) return is;
    }
    finished_ = true;
    if (lock_.owns_lock()) lock_.unlock();
    // Pages allocated during this transaction were never referenced by any
    // committed meta page, so they are simply orphaned space -- the next
    // process to open this file reloads nextFreePageId_ from the last
    // *committed* meta and naturally never revisits them. No page-by-page
    // rollback bookkeeping is required for correctness (see
    // PageManager::allocatePage / the growth-model notes in page_manager.hpp).

    // A provisionally-created sub-database handle never became durable, so
    // it must be freed; a staged drop was likewise never committed and
    // needs no action (the handle remains exactly as valid as before).
    if (subDbCatalog_ != nullptr) {
        for (auto& [h, entry] : subDbs_) {
            if (entry.isNew) subDbCatalog_->rollbackPendingCreate(h);
        }
    }
    subDbs_.clear();
    return Status::OK();
}

Status WriteTransaction::beginNested() {
    if (finished_) return Status::Internal("transaction already finished");
    // LMDB allows only one active child writer path; we model that as a stack
    // of savepoints (child may itself beginNested).
    Savepoint sp;
    sp.masterRootPageId = masterRootPageId_;
    sp.retirementLogMark = pm_->retirementLogSize();
    savepoints_.push_back(sp);
    return Status::OK();
}

Status WriteTransaction::commitNested() {
    if (finished_) return Status::Internal("transaction already finished");
    if (savepoints_.empty()) {
        return Status::InvalidArgument("no nested transaction to commit");
    }
    // Keep current masterRootPageId_ and retirements — they become part of the parent.
    savepoints_.pop_back();
    return Status::OK();
}

Status WriteTransaction::abortNested() {
    if (finished_) return Status::Internal("transaction already finished");
    if (savepoints_.empty()) {
        return Status::InvalidArgument("no nested transaction to abort");
    }
    Savepoint sp = savepoints_.back();
    savepoints_.pop_back();
    masterRootPageId_ = sp.masterRootPageId;
    pm_->rollbackRetirementsTo(sp.retirementLogMark, txnId_);
    // Any write-bound Cursor opened during the now-discarded nested scope may
    // still hold a stack_ pointing at pages the restored root no longer
    // reaches; bump generation_ so its next call fails instead of silently
    // returning rolled-back data (see Cursor::checkGeneration).
    ++generation_;
    return Status::OK();
}

}  // namespace annpocketdb::txn
