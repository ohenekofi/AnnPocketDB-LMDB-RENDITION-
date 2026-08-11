#include "annpocketdb/api/sub_database.hpp"

#include <cstring>
#include <utility>

#include "annpocketdb/btree/node_codec.hpp"
#include "annpocketdb/index/index.hpp"  // reuse IndexManager::collectReachablePages for tree-walk retire

namespace annpocketdb::api {

// ---------------------------------------------------------------------------
// Catalog entry encoding (one entry per key -- see kSubDbKeyPrefix)
// ---------------------------------------------------------------------------

Status encodeSubDbEntry(PageId rootPageId, btree::DbFlags flags, ByteBuffer& out) {
    out.resize(sizeof(rootPageId) + sizeof(std::uint32_t));
    std::memcpy(out.data(), &rootPageId, sizeof(rootPageId));
    std::uint32_t flagBits = static_cast<std::uint32_t>(flags);
    std::memcpy(out.data() + sizeof(rootPageId), &flagBits, sizeof(flagBits));
    return Status::OK();
}

Status decodeSubDbEntry(ByteView data, PageId& rootPageId, btree::DbFlags& flags) {
    if (data.size() < sizeof(rootPageId)) return Status::Corruption("subdb entry too short");
    std::memcpy(&rootPageId, data.data(), sizeof(rootPageId));
    if (data.size() >= sizeof(rootPageId) + sizeof(std::uint32_t)) {
        std::uint32_t flagBits = 0;
        std::memcpy(&flagBits, data.data() + sizeof(rootPageId), sizeof(flagBits));
        flags = static_cast<btree::DbFlags>(flagBits);
    } else {
        flags = btree::DbFlags::None;  // legacy/short entry: no flags recorded
    }
    return Status::OK();
}

// ---------------------------------------------------------------------------
// SubDatabaseManager
// ---------------------------------------------------------------------------

SubDatabaseManager::SubDatabaseManager(core::PageManager& pm, btree::BTree& masterTree,
                                       std::uint32_t maxNamedDatabases)
    : maxNamedDatabases_(maxNamedDatabases), pm_(pm), masterTree_(masterTree) {
    slots_.emplace_back();  // index 0 reserved: kInvalidSubDbHandle
}

Result<txn::ReadTransaction> SubDatabaseManager::beginInternalRead() const {
    core::Snapshot snap = pm_.currentSnapshot();
    core::ReaderHandle handle = pm_.registerReader(snap.txnId);
    if (handle == core::kInvalidReaderHandle) {
        return Status::ReadersFull("reader table full; too many concurrent readers across processes");
    }
    return Result<txn::ReadTransaction>(txn::ReadTransaction(pm_, masterTree_, snap, handle));
}

SubDbHandle SubDatabaseManager::allocateSlotLocked(const std::string& name, btree::DbFlags flags) {
    SubDbHandle h;
    if (!freeSlots_.empty()) {
        h = freeSlots_.back();
        freeSlots_.pop_back();
    } else {
        h = static_cast<SubDbHandle>(slots_.size());
        slots_.emplace_back();
    }
    slots_[h].name = name;
    slots_[h].inUse = true;
    slots_[h].flags = flags;
    slots_[h].keyCmp = nullptr;
    slots_[h].valueCmp = nullptr;
    byName_[name] = h;
    return h;
}

void SubDatabaseManager::invalidateSlotLocked(SubDbHandle h) {
    byName_.erase(slots_[h].name);
    slots_[h].inUse = false;
    slots_[h].sequence++;
    freeSlots_.push_back(h);
}

Result<SubDbHandle> SubDatabaseManager::open(const std::string& name) {
    {
        std::shared_lock lock(mutex_);
        auto it = byName_.find(name);
        if (it != byName_.end()) return Result<SubDbHandle>(it->second);
    }

    auto rt = beginInternalRead();
    if (!rt) return rt.status();
    txn::ReadTransaction rtxn = rt.moveValue();
    auto r = rtxn.get(toByteView(makeSubDbKey(name)));
    if (!r) {
        if (r.status().code() == StatusCode::NotFound) return Status::NotFound("sub-database not found");
        return r.status();
    }
    PageId sanityRoot = kInvalidPageId;
    btree::DbFlags flags = btree::DbFlags::None;
    Status ds = decodeSubDbEntry(toByteView(r.value()), sanityRoot, flags);
    if (!ds.ok()) return ds;

    std::unique_lock lock(mutex_);
    // Re-check: another thread may have raced us between the shared-lock
    // miss above and acquiring the exclusive lock here.
    auto it = byName_.find(name);
    if (it != byName_.end()) return Result<SubDbHandle>(it->second);
    SubDbHandle h = allocateSlotLocked(name, flags);
    return Result<SubDbHandle>(h);
}

Result<SubDbHandle> SubDatabaseManager::create(txn::WriteTransaction& txn, const std::string& name,
                                               btree::DbFlags flags) {
    {
        std::shared_lock lock(mutex_);
        if (byName_.find(name) != byName_.end()) return Status::KeyExists("sub-database already exists");
    }
    // byName_ only reflects names opened/created in this process; also check
    // the on-disk catalog directly through the caller's transaction.
    auto existing = txn.get(toByteView(makeSubDbKey(name)));
    if (existing.ok()) return Status::KeyExists("sub-database already exists");
    if (existing.status().code() != StatusCode::NotFound) return existing.status();

    std::unique_lock lock(mutex_);
    if (byName_.find(name) != byName_.end()) return Status::KeyExists("sub-database already exists");
    std::size_t openCount = slots_.size() - 1 - freeSlots_.size();
    if (maxNamedDatabases_ != 0 && openCount >= maxNamedDatabases_) {
        return Status::DbsFull(
            "maxNamedDatabases limit reached (" + std::to_string(maxNamedDatabases_) + ")");
    }

    auto alloc = pm_.allocatePage(txn.txnId(), core::PageType::Leaf);
    if (!alloc) return alloc.status();
    core::PageHeader* header = alloc.value();
    pm_.finalizePage(header, 0, 0);
    PageId root = header->pageId;

    SubDbHandle h = allocateSlotLocked(name, flags);
    std::uint32_t seq = slots_[h].sequence;
    lock.unlock();

    ByteBuffer key = makeSubDbKey(name);
    ByteBuffer entry;
    Status es = encodeSubDbEntry(root, flags, entry);
    if (!es.ok()) {
        rollbackPendingCreate(h);
        return es;
    }
    Status ps = txn.put(toByteView(key), toByteView(entry), true);
    if (!ps.ok()) {
        rollbackPendingCreate(h);
        return ps;
    }
    // Usable for the rest of this transaction immediately (LMDB's DB_NEW);
    // published for everyone else only once `txn` durably commits, and
    // rolled back automatically if it doesn't (see WriteTransaction::commit/abort).
    txn.registerNewSubDb(h, root, seq, key);
    return Result<SubDbHandle>(h);
}

Status SubDatabaseManager::drop(txn::WriteTransaction& txn, SubDbHandle h) {
    std::string name;
    {
        std::shared_lock lock(mutex_);
        if (h == kInvalidSubDbHandle || h >= slots_.size() || !slots_[h].inUse) {
            return Status::NotFound("sub-database not found");
        }
        name = slots_[h].name;
    }
    ByteBuffer key = makeSubDbKey(name);

    auto got = txn.get(toByteView(key));
    if (!got) return got.status();
    PageId root = kInvalidPageId;
    btree::DbFlags existingFlags = btree::DbFlags::None;
    Status ds = decodeSubDbEntry(toByteView(got.value()), root, existingFlags);
    if (!ds.ok()) return ds;
    (void)existingFlags;  // drop() removes the whole record; flags irrelevant here

    std::vector<PageId> pages;
    Status cs = index::IndexManager::collectReachablePages(pm_, root, pages);
    if (!cs.ok()) return cs;
    for (PageId pid : pages) pm_.retirePage(pid, txn.txnId());

    Status rs = txn.remove(toByteView(key));
    if (!rs.ok()) return rs;
    txn.markSubDbDropped(h);
    return Status::OK();
}

Result<PageId> SubDatabaseManager::empty(txn::WriteTransaction& txn, SubDbHandle h) {
    std::string name;
    {
        std::shared_lock lock(mutex_);
        if (h == kInvalidSubDbHandle || h >= slots_.size() || !slots_[h].inUse) {
            return Status::NotFound("sub-database not found");
        }
        name = slots_[h].name;
    }
    ByteBuffer key = makeSubDbKey(name);

    auto got = txn.get(toByteView(key));
    if (!got) return got.status();
    PageId oldRoot = kInvalidPageId;
    btree::DbFlags existingFlags = btree::DbFlags::None;
    Status ds = decodeSubDbEntry(toByteView(got.value()), oldRoot, existingFlags);
    if (!ds.ok()) return ds;

    std::vector<PageId> pages;
    Status cs = index::IndexManager::collectReachablePages(pm_, oldRoot, pages);
    if (!cs.ok()) return cs;
    for (PageId pid : pages) pm_.retirePage(pid, txn.txnId());

    auto alloc = pm_.allocatePage(txn.txnId(), core::PageType::Leaf);
    if (!alloc) return alloc.status();
    core::PageHeader* header = alloc.value();
    pm_.finalizePage(header, 0, 0);
    PageId newRoot = header->pageId;

    // Preserve the existing flags -- clear() empties contents, it must not
    // reset DupSort/DupFixed/ReverseKey/etc. back to defaults.
    ByteBuffer entry;
    Status es = encodeSubDbEntry(newRoot, existingFlags, entry);
    if (!es.ok()) return es;
    Status ps = txn.put(toByteView(key), toByteView(entry), true);
    if (!ps.ok()) return ps;
    // No manager-side bookkeeping needed: this is an ordinary catalog-record
    // update, already durable via the normal transaction commit.
    return Result<PageId>(newRoot);
}

void SubDatabaseManager::close(SubDbHandle h) {
    std::unique_lock lock(mutex_);
    if (h == kInvalidSubDbHandle || h >= slots_.size() || !slots_[h].inUse) return;
    invalidateSlotLocked(h);
}

Result<std::vector<std::string>> SubDatabaseManager::list(txn::ReadTransaction& txn) const {
    std::vector<std::string> names;
    btree::Cursor cursor = txn.openCursor();
    ByteView prefix = toByteView(kSubDbKeyPrefix);
    auto r = cursor.seek(prefix);
    if (!r) return r.status();
    while (r.value()) {
        auto k = cursor.key();
        if (!k) return k.status();
        ByteView key = k.value();
        if (key.size() < prefix.size() ||
            compareBytes(ByteView(key.data(), prefix.size()), prefix) != 0) {
            break;  // walked past the prefix range
        }
        names.emplace_back(reinterpret_cast<const char*>(key.data() + prefix.size()),
                           key.size() - prefix.size());
        r = cursor.next();
        if (!r) return r.status();
    }
    return Result<std::vector<std::string>>(std::move(names));
}

std::uint32_t SubDatabaseManager::currentSequence(SubDbHandle h) const {
    std::shared_lock lock(mutex_);
    if (h == kInvalidSubDbHandle || h >= slots_.size()) return 0;
    return slots_[h].sequence;
}

Result<ByteBuffer> SubDatabaseManager::validateHandle(SubDbHandle h, std::uint32_t openSeq) {
    std::shared_lock lock(mutex_);
    if (h == kInvalidSubDbHandle || h >= slots_.size() || !slots_[h].inUse ||
        slots_[h].sequence != openSeq) {
        return Status::BadDbi("stale sub-database handle");
    }
    return Result<ByteBuffer>(makeSubDbKey(slots_[h].name));
}

bool SubDatabaseManager::sequenceMatches(SubDbHandle h, std::uint32_t seq) const {
    std::shared_lock lock(mutex_);
    if (h == kInvalidSubDbHandle || h >= slots_.size() || !slots_[h].inUse) return false;
    return slots_[h].sequence == seq;
}

Status SubDatabaseManager::decodeCatalogRoot(ByteView entryBytes, PageId& outRoot) const {
    btree::DbFlags ignoredFlags;
    return decodeSubDbEntry(entryBytes, outRoot, ignoredFlags);
}

Status SubDatabaseManager::encodeCatalogRoot(SubDbHandle h, PageId root, ByteBuffer& out) const {
    // Preserve this handle's already-persisted flags -- an ordinary
    // put/remove root update must not reset DupSort/DupFixed/ReverseKey/etc.
    // back to defaults.
    return encodeSubDbEntry(root, flagsOf(h), out);
}

btree::DbFlags SubDatabaseManager::flagsOf(SubDbHandle h) const {
    std::shared_lock lock(mutex_);
    if (h == kInvalidSubDbHandle || h >= slots_.size() || !slots_[h].inUse) return btree::DbFlags::None;
    return slots_[h].flags;
}

btree::KeyOrder SubDatabaseManager::keyOrderOf(SubDbHandle h) const {
    std::shared_lock lock(mutex_);
    if (h == kInvalidSubDbHandle || h >= slots_.size() || !slots_[h].inUse) {
        return btree::KeyOrder::fromFlags(btree::DbFlags::None);
    }
    btree::KeyOrder order = btree::KeyOrder::fromFlags(slots_[h].flags);
    order.customKeyCmp = slots_[h].keyCmp;
    order.customValueCmp = slots_[h].valueCmp;
    return order;
}

void SubDatabaseManager::setKeyComparator(SubDbHandle h, Comparator cmp) {
    std::unique_lock lock(mutex_);
    if (h == kInvalidSubDbHandle || h >= slots_.size() || !slots_[h].inUse) return;
    slots_[h].keyCmp = std::move(cmp);
}

void SubDatabaseManager::setValueComparator(SubDbHandle h, Comparator cmp) {
    std::unique_lock lock(mutex_);
    if (h == kInvalidSubDbHandle || h >= slots_.size() || !slots_[h].inUse) return;
    slots_[h].valueCmp = std::move(cmp);
}

void SubDatabaseManager::confirmDrop(SubDbHandle h) {
    std::unique_lock lock(mutex_);
    if (h == kInvalidSubDbHandle || h >= slots_.size() || !slots_[h].inUse) return;
    invalidateSlotLocked(h);
}

void SubDatabaseManager::rollbackPendingCreate(SubDbHandle h) {
    std::unique_lock lock(mutex_);
    if (h == kInvalidSubDbHandle || h >= slots_.size() || !slots_[h].inUse) return;
    invalidateSlotLocked(h);
}

// ---------------------------------------------------------------------------
// SubDatabaseWriter
// ---------------------------------------------------------------------------

SubDatabaseWriter::SubDatabaseWriter(txn::WriteTransaction wtxn, SubDatabase* owner)
    : wtxn_(std::move(wtxn)), owner_(owner) {}

SubDatabaseWriter::~SubDatabaseWriter() {
    if (!finished_) abort();
}

SubDatabaseWriter::SubDatabaseWriter(SubDatabaseWriter&& o) noexcept
    : wtxn_(std::move(o.wtxn_)), owner_(o.owner_), finished_(o.finished_) {
    o.finished_ = true;
    o.owner_ = nullptr;
}

SubDatabaseWriter& SubDatabaseWriter::operator=(SubDatabaseWriter&& o) noexcept {
    if (this != &o) {
        if (!finished_) abort();
        wtxn_ = std::move(o.wtxn_);
        owner_ = o.owner_;
        finished_ = o.finished_;
        o.finished_ = true;
        o.owner_ = nullptr;
    }
    return *this;
}

Status SubDatabaseWriter::put(ByteView key, ByteView value, bool overwrite) {
    if (finished_) return Status::Internal("SubDatabaseWriter already finished");
    if (!owner_) return Status::Internal("SubDatabaseWriter has no owner");
    return wtxn_.putSubDb(owner_->handle(), owner_->openSeq(), key, value, overwrite);
}

Status SubDatabaseWriter::remove(ByteView key) {
    if (finished_) return Status::Internal("SubDatabaseWriter already finished");
    if (!owner_) return Status::Internal("SubDatabaseWriter has no owner");
    return wtxn_.removeSubDb(owner_->handle(), owner_->openSeq(), key);
}

Result<ByteBuffer> SubDatabaseWriter::get(ByteView key) const {
    if (finished_) return Status::Internal("SubDatabaseWriter already finished");
    if (!owner_) return Status::Internal("SubDatabaseWriter has no owner");
    return wtxn_.getSubDb(owner_->handle(), owner_->openSeq(), key);
}

Result<bool> SubDatabaseWriter::contains(ByteView key) const {
    if (finished_) return Status::Internal("SubDatabaseWriter already finished");
    if (!owner_) return Status::Internal("SubDatabaseWriter has no owner");
    return wtxn_.containsSubDb(owner_->handle(), owner_->openSeq(), key);
}

btree::Cursor SubDatabaseWriter::openCursor() const {
    return wtxn_.openCursorSubDb(owner_->handle(), owner_->openSeq());
}

Status SubDatabaseWriter::commit(core::SyncMode mode) {
    if (finished_) return Status::Internal("SubDatabaseWriter already finished");
    // All catalog export/rollback bookkeeping now happens generically inside
    // WriteTransaction::commit()/abort() via its per-transaction sub-database
    // table -- nothing sub-database-specific left to do here.
    Status s = wtxn_.commit(mode);
    finished_ = true;
    return s;
}

void SubDatabaseWriter::abort() {
    if (finished_) return;
    wtxn_.abort();
    finished_ = true;
}

PageId SubDatabaseWriter::rootPageId() const {
    return owner_ ? wtxn_.currentSubDbRoot(owner_->handle()) : kInvalidPageId;
}

// ---------------------------------------------------------------------------
// SubDatabase
// ---------------------------------------------------------------------------

Result<txn::ReadTransaction> SubDatabase::beginRead() {
    if (!pm_ || !tree_ || !manager_) return Status::Internal("sub-database not initialized");
    core::Snapshot snap = pm_->currentSnapshot();
    core::ReaderHandle handle = pm_->registerReader(snap.txnId);
    if (handle == core::kInvalidReaderHandle) {
        return Status::ReadersFull("reader table full; too many concurrent readers across processes");
    }
    txn::ReadTransaction rtxn(*pm_, *tree_, snap, handle);
    rtxn.attachSubDbCatalog(manager_);
    Status s = rtxn.rebindToSubDb(handle_, openSeq_);
    if (!s.ok()) return s;
    return Result<txn::ReadTransaction>(std::move(rtxn));
}

Result<SubDatabaseWriter> SubDatabase::beginWriteTxn() {
    if (!pm_ || !tree_ || !manager_) return Status::Internal("sub-database not initialized");
    txn::WriteTransaction wtxn(*pm_, *tree_);
    wtxn.attachSubDbCatalog(manager_);
    auto r = wtxn.useSubDb(handle_, openSeq_);
    if (!r) {
        wtxn.abort();
        return r.status();
    }
    return Result<SubDatabaseWriter>(SubDatabaseWriter(std::move(wtxn), this));
}

Result<txn::WriteTransaction> SubDatabase::beginWrite() {
    if (!pm_ || !tree_ || !manager_) return Status::Internal("sub-database not initialized");
    if (!manager_->sequenceMatches(handle_, openSeq_)) {
        return Status::BadDbi("stale sub-database handle");
    }
    return Result<txn::WriteTransaction>(txn::WriteTransaction(*pm_, *tree_));
}

Result<ByteBuffer> SubDatabase::get(ByteView key) {
    auto txnResult = beginRead();
    if (!txnResult) return txnResult.status();
    // getSubDb (not the plain rebind-based get()) so this handle's
    // persisted flags/custom comparator govern the lookup -- rebindToSubDb
    // inside beginRead() already cached this handle's entry, so this is a
    // cheap cache hit, not a second catalog round-trip.
    return txnResult.value().getSubDb(handle_, openSeq_, key);
}

Status SubDatabase::put(ByteView key, ByteView value, bool overwrite, core::SyncMode mode) {
    auto wr = beginWriteTxn();
    if (!wr) return wr.status();
    SubDatabaseWriter w = wr.moveValue();
    Status s = w.put(key, value, overwrite);
    if (!s.ok()) {
        w.abort();
        return s;
    }
    return w.commit(mode);
}

Status SubDatabase::remove(ByteView key, core::SyncMode mode) {
    auto wr = beginWriteTxn();
    if (!wr) return wr.status();
    SubDatabaseWriter w = wr.moveValue();
    Status s = w.remove(key);
    if (!s.ok()) {
        w.abort();
        return s;
    }
    return w.commit(mode);
}

Status SubDatabase::clear(core::SyncMode mode) {
    if (!pm_ || !tree_ || !manager_) return Status::Internal("sub-database not initialized");
    txn::WriteTransaction wtxn(*pm_, *tree_);
    wtxn.attachSubDbCatalog(manager_);
    auto newRoot = manager_->empty(wtxn, handle_);
    if (!newRoot) {
        wtxn.abort();
        return newRoot.status();
    }
    return wtxn.commit(mode);
}

void SubDatabase::close() {
    if (manager_ && handle_ != kInvalidSubDbHandle) {
        manager_->close(handle_);
    }
    handle_ = kInvalidSubDbHandle;
}

}  // namespace annpocketdb::api
