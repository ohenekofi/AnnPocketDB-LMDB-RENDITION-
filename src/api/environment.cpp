#include "annpocketdb/api/environment.hpp"
#include <algorithm>
#include "annpocketdb/wal/wal.hpp"
#include "annpocketdb/btree/node_codec.hpp"

namespace annpocketdb::api {

Result<Environment> Environment::open(const std::filesystem::path& path, const EnvironmentOptions& options) {
    core::PageManagerOptions pmOpts;
    pmOpts.pageSize = options.pageSize;
    pmOpts.maxDatabaseSize = options.maxDatabaseSize;
    pmOpts.initialFileSize = options.initialFileSize;
    pmOpts.readOnly = options.readOnly;
    pmOpts.multiProcess = options.multiProcess;
    pmOpts.durableCrc = options.durableCrc;
    pmOpts.durability.noSync = options.noSync;
    pmOpts.durability.noMetaSync = options.noMetaSync;
    pmOpts.durability.mapAsync = options.mapAsync;
    pmOpts.maxReaders = options.maxReaders;
    pmOpts.maxNamedDatabases = options.maxNamedDatabases;
#if defined(ANNPOCKETDB_WITH_ENCRYPTION)
    pmOpts.encryptionEnabled = options.enableEncryption;
    pmOpts.encryptionKey = options.encryptionKey;
    pmOpts.sha256ChecksumEnabled = options.enableSha256Checksum;
#else
    if (options.enableEncryption) {
        return Status::NotSupported("encryption support was not built (ANNPOCKETDB_WITH_ENCRYPTION)");
    }
    if (options.enableSha256Checksum) {
        return Status::NotSupported("SHA-256 checksum support was not built (ANNPOCKETDB_WITH_ENCRYPTION)");
    }
#endif
#if defined(ANNPOCKETDB_WITH_COMPRESSION)
    pmOpts.compressionEnabled = options.enableCompression;
    pmOpts.compressionMinSize = options.compressionMinSize;
#else
    if (options.enableCompression) {
        return Status::NotSupported("compression support was not built (ANNPOCKETDB_WITH_COMPRESSION)");
    }
#endif

    auto pmResult = core::PageManager::open(path, pmOpts);
    if (!pmResult) return pmResult.status();

    Environment env;
    env.pm_ = pmResult.moveValue();
    env.tree_ = std::make_unique<btree::BTree>(*env.pm_);
    env.subDbManager_ = std::make_unique<SubDatabaseManager>(*env.pm_, *env.tree_, options.maxNamedDatabases);
    env.options_ = options;

#if defined(ANNPOCKETDB_WITH_WAL) && ANNPOCKETDB_WITH_WAL
    if (options.enableWal) {
        std::filesystem::path walPath = options.walPath.empty()
                                            ? std::filesystem::path(path.string() + ".wal")
                                            : options.walPath;
        auto walResult = wal::Wal::open(walPath);
        if (!walResult) return walResult.status();
        env.wal_ = walResult.moveValue();

        // Production recovery order:
        //   1. recover() with WAL *not* attached (no re-logging during redo)
        //   2. install meta from last Commit when newer than on-disk meta
        //      (done inside recover via installRecoveredCommit)
        //   3. truncate WAL after a successful install so the next open is idle
        //   4. attachWal for subsequent commits
        auto recovered = env.wal_->recover(*env.pm_);
        if (!recovered) return recovered.status();

        if (recovered.value().appliedCommit) {
            Status ts = env.wal_->truncateAfterRecovery();
            if (!ts.ok()) return ts;
        }

        if (!options.readOnly) {
            env.pm_->attachWal(env.wal_.get());
        }
    }
#else
    if (options.enableWal) {
        return Status::NotSupported("WAL support was not built (ANNPOCKETDB_WITH_WAL)");
    }
#endif


    if (options.enableChangefeed) {
        auto cf = changefeed::createDurableChangeFeed(options.changefeedAsync);
        if (!cf) return cf.status();
        env.changeFeed_ = cf.moveValue();
    }

    return Result<Environment>(std::move(env));
}

Result<txn::ReadTransaction> Environment::beginRead() {
    core::Snapshot snap = pm_->snapshotForNewRead();
    core::ReaderHandle handle = pm_->registerReader(snap.txnId);
    if (handle == core::kInvalidReaderHandle) {
        // Shared reader table full (LMDB-equivalent of MDB_READERS_FULL).
        return Status::ReadersFull("reader table full; too many concurrent readers across processes");
    }
    txn::ReadTransaction rtxn(*pm_, *tree_, snap, handle);
    rtxn.attachSubDbCatalog(subDbManager_.get());
    return Result<txn::ReadTransaction>(std::move(rtxn));
}

Result<txn::WriteTransaction> Environment::beginWrite() {
    if (options_.readOnly) return Status::ReadOnly("environment was opened read-only");
    txn::WriteTransaction wtxn(*pm_, *tree_);
    wtxn.attachSubDbCatalog(subDbManager_.get());
    if (changeFeed_) {
        wtxn.setChangeSink(changeFeed_->sink());
    }
    return Result<txn::WriteTransaction>(std::move(wtxn));
}

Result<ByteBuffer> Environment::get(ByteView key) {
    auto txnResult = beginRead();
    if (!txnResult) return txnResult.status();
    return txnResult.value().get(key);
}

Status Environment::put(ByteView key, ByteView value, bool overwrite, core::SyncMode mode) {
    auto txnResult = beginWrite();
    if (!txnResult) return txnResult.status();
    txn::WriteTransaction wtxn = txnResult.moveValue();
    Status s = wtxn.put(key, value, overwrite);
    if (!s.ok()) {
        wtxn.abort();
        return s;
    }
    return wtxn.commit(mode);
}

Status Environment::remove(ByteView key, core::SyncMode mode) {
    auto txnResult = beginWrite();
    if (!txnResult) return txnResult.status();
    txn::WriteTransaction wtxn = txnResult.moveValue();
    Status s = wtxn.remove(key);
    if (!s.ok()) {
        wtxn.abort();
        return s;
    }
    return wtxn.commit(mode);
}


Result<SubDatabase> Environment::createSubDatabase(const std::string& name, btree::DbFlags flags,
                                                   core::SyncMode mode) {
    if (options_.readOnly) return Status::ReadOnly("environment was opened read-only");
    if (name.empty()) return Status::InvalidArgument("sub-database name empty");
    if (name.find('\0') != std::string::npos) return Status::InvalidArgument("sub-database name contains NUL");

    auto wt = beginWrite();
    if (!wt) return wt.status();
    txn::WriteTransaction wtxn = wt.moveValue();

    auto created = subDbManager_->create(wtxn, name, flags);
    if (!created) {
        wtxn.abort();
        return created.status();
    }
    SubDbHandle h = created.value();

    // On failure, wtxn.commit() itself rolls the provisional handle back
    // (see WriteTransaction::commit's sub-database export/rollback loop).
    Status s = wtxn.commit(mode);
    if (!s.ok()) return s;

    std::uint32_t seq = subDbManager_->currentSequence(h);
    return Result<SubDatabase>(SubDatabase(pm_.get(), tree_.get(), subDbManager_.get(), name, h, seq));
}

Result<SubDatabase> Environment::openSubDatabase(const std::string& name) {
    auto h = subDbManager_->open(name);
    if (!h) return h.status();
    std::uint32_t seq = subDbManager_->currentSequence(h.value());
    return Result<SubDatabase>(SubDatabase(pm_.get(), tree_.get(), subDbManager_.get(), name, h.value(), seq));
}

Status Environment::sync(bool force) {
    core::SyncMode mode = core::SyncMode::Durable;
    if (!force) {
        if (options_.noSync) mode = core::SyncMode::NoSync;
        else if (options_.mapAsync) mode = core::SyncMode::Flush;
    }
    return pm_->flushAll(mode);
}

std::size_t Environment::maxKeySize() const { return tree_->maxKeySize(); }

Result<SubDatabase> Environment::openOrCreateSubDatabase(const std::string& name, btree::DbFlags flags,
                                                          core::SyncMode mode) {
    auto opened = openSubDatabase(name);
    if (opened.ok()) return opened;
    if (opened.status().code() != StatusCode::NotFound) return opened.status();
    // Flags only apply at first creation -- matches LMDB's open_db()
    // ignoring them for an already-existing database.
    auto created = createSubDatabase(name, flags, mode);
    if (created.ok()) return created;
    // Lost a race with another writer that created it between our open()
    // miss and this create() attempt: fall back to opening what they made.
    if (created.status().code() == StatusCode::KeyExists) return openSubDatabase(name);
    return created.status();
}

Result<std::vector<std::string>> Environment::listSubDatabases() {
    auto rt = beginRead();
    if (!rt) return rt.status();
    txn::ReadTransaction rtxn = rt.moveValue();
    return subDbManager_->list(rtxn);
}

Status Environment::dropSubDatabase(const std::string& name, core::SyncMode mode) {
    if (options_.readOnly) return Status::ReadOnly("environment was opened read-only");

    auto h = subDbManager_->open(name);
    if (!h) return h.status();

    auto wt = beginWrite();
    if (!wt) return wt.status();
    txn::WriteTransaction wtxn = wt.moveValue();

    Status s = subDbManager_->drop(wtxn, h.value());
    if (!s.ok()) {
        wtxn.abort();
        return s;
    }
    // On success, wtxn.commit() itself invalidates the handle for everyone
    // (see WriteTransaction::commit's sub-database export loop, driven by
    // the markSubDbDropped() call inside SubDatabaseManager::drop above).
    return wtxn.commit(mode);
}


Result<blob::BlobRef> Environment::putBlob(ByteView key, ByteView data, core::SyncMode mode) {
    auto wt = beginWrite();
    if (!wt) return wt.status();
    txn::WriteTransaction wtxn = wt.moveValue();

    // Replace any previous blob chain bound to this key.
    auto prev = wtxn.get(key);
    if (prev.ok() && blob::isBlobRef(toByteView(prev.value()))) {
        blob::BlobRef oldRef;
        if (blob::decodeBlobRef(toByteView(prev.value()), oldRef).ok()) {
            blob::BlobStore store(*pm_);
            Status rs = store.remove(oldRef.id, wtxn.txnId());
            if (!rs.ok()) {
                wtxn.abort();
                return rs;
            }
        }
    }

    blob::BlobStore store(*pm_);
    auto refR = store.put(data, wtxn.txnId());
    if (!refR) {
        wtxn.abort();
        return refR.status();
    }
    ByteBuffer refVal = blob::encodeBlobRef(refR.value());
    Status s = wtxn.put(key, toByteView(refVal), true);
    if (!s.ok()) {
        wtxn.abort();
        return s;
    }
    s = wtxn.commit(mode);
    if (!s.ok()) return s;
    return Result<blob::BlobRef>(refR.value());
}

Result<ByteBuffer> Environment::getBlob(ByteView key) {
    auto g = get(key);
    if (!g) return g.status();
    ByteView val = toByteView(g.value());
    if (!blob::isBlobRef(val)) {
        // Inline value — return as-is for convenience.
        return Result<ByteBuffer>(g.moveValue());
    }
    blob::BlobRef ref;
    Status s = blob::decodeBlobRef(val, ref);
    if (!s.ok()) return s;
    blob::BlobStore store(*pm_);
    return store.get(ref.id);
}

Status Environment::removeBlob(ByteView key, core::SyncMode mode) {
    auto wt = beginWrite();
    if (!wt) return wt.status();
    txn::WriteTransaction wtxn = wt.moveValue();

    auto prev = wtxn.get(key);
    if (!prev) {
        wtxn.abort();
        return prev.status();
    }
    if (blob::isBlobRef(toByteView(prev.value()))) {
        blob::BlobRef ref;
        Status s = blob::decodeBlobRef(toByteView(prev.value()), ref);
        if (!s.ok()) {
            wtxn.abort();
            return s;
        }
        blob::BlobStore store(*pm_);
        s = store.remove(ref.id, wtxn.txnId());
        if (!s.ok()) {
            wtxn.abort();
            return s;
        }
    }
    Status s = wtxn.remove(key);
    if (!s.ok()) {
        wtxn.abort();
        return s;
    }
    return wtxn.commit(mode);
}

Status Environment::rollbackPrepared() {
    if (!pm_) return Status::InvalidArgument("environment not open");
    int slot = pm_->findLingeringPreparedSlot();
    if (slot < 0) return Status::NotFound("no lingering prepared transaction found");
    core::PageManager::PreparedHandle handle{slot};
    return pm_->invalidatePreparedSlot(handle, core::SyncMode::Durable);
}

Status Environment::close() {
    if (pm_) pm_->attachWal(nullptr);
    subDbManager_.reset();  // holds a BTree& into tree_; must not outlive it
    tree_.reset();
    wal_.reset();
    if (pm_) return pm_->close();
    return Status::OK();
}


Status Environment::setMapSize(std::uint64_t newMaxBytes) {
    if (!pm_) return Status::InvalidArgument("environment not open");
    return pm_->setMapSize(newMaxBytes);
}

std::uint64_t Environment::mapSize() const {
    return pm_ ? pm_->mapSize() : 0;
}


Result<DbStat> Environment::stat() const {
    if (!pm_) return Status::InvalidArgument("environment not open");
    return stat(pm_->currentSnapshot().masterRootPageId);
}

Result<DbStat> Environment::stat(PageId rootPageId) const {
    if (!pm_ || !tree_) return Status::InvalidArgument("environment not open");
    auto tr = tree_->collectStat(rootPageId);
    if (!tr) return tr.status();
    const auto& ts = tr.value();
    DbStat s;
    s.pageSize = pm_->pageSize();
    s.depth = ts.depth;
    s.branchPages = ts.branchPages;
    s.leafPages = ts.leafPages;
    s.overflowPages = ts.overflowPages;
    s.entries = ts.entries;
    s.freePages = pm_->freeList().reusableCount() + pm_->freeList().retiredCount();
    s.pagesUsed = ts.branchPages + ts.leafPages + ts.overflowPages;
    return Result<DbStat>(s);
}

EnvInfo Environment::info() const {
    EnvInfo i;
    if (!pm_) return i;
    i.mapSize = pm_->mapSize();
    PageId next = pm_->nextFreePageId();
    i.lastPageId = next == 0 ? 0 : next - 1;
    i.lastTxnId = pm_->lastCommittedTxnId();
    i.maxReaders = static_cast<std::uint32_t>(
        std::min<std::size_t>(pm_->writerMutex().maxReaders(), 0xffffffffu));
    i.numReaders = static_cast<std::uint32_t>(pm_->activeReaderCount());
    i.pageSize = pm_->pageSize();
    i.fileSize = pm_->mappedFile().fileSize();
    i.multiProcess = options_.multiProcess;
    return i;
}

std::size_t Environment::readerCheck() {
    if (!pm_) return 0;
    return pm_->writerMutex().readerCheck();
}

std::vector<ReaderInfo> Environment::readerList() const {
    std::vector<ReaderInfo> out;
    if (!pm_) return out;
    for (const auto& s : pm_->writerMutex().readerList()) {
        ReaderInfo r;
        r.slot = s.slot;
        r.pid = s.pid;
        r.snapshotTxnId = s.snapshotTxnId;
        out.push_back(r);
    }
    return out;
}

}  // namespace annpocketdb::api

