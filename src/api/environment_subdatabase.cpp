//
// AnnPocketDb :: api/environment_subdatabase.cpp
//
// Environment's named-sub-database methods -- split out of environment.cpp
// (kernel tier) into the features tier, alongside sub_database.cpp, so that
// annpocketdb_kernel never needs a symbol from annpocketdb_features (a
// static archive must never need a symbol from an archive that depends on
// it). sub_database.cpp itself calls index::IndexManager::collectReachable
// Pages (a *different* feature module), so unlike WAL/changefeed it can't
// simply move into the kernel tier without cascading into needing index
// (and, transitively, document) there too -- moving just the call sites
// instead avoids that.
//
// Environment::open() used to construct SubDatabaseManager directly (a
// kernel .cpp calling a features-tier constructor); it no longer does.
// ensureSubDbManager() below constructs it lazily, on first actual use.
// SubDatabaseManager's constructor does no I/O or fallible work (see
// sub_database.cpp) -- it only stores two references and reserves slot 0 --
// so deferring it is fully behavior-preserving: nothing that could
// previously fail at Environment::open() time now fails later instead.
//
// beginRead()/beginWrite() (still in environment.cpp) keep calling
// attachSubDbCatalog(subDbManager_.get()) unchanged -- that only stores a
// pointer (possibly still null) through the kernel-owned ISubDbCatalog
// interface, never a direct call, so it was never actually part of the
// circular-dependency problem. A SubDbHandle can only ever be obtained
// through one of the methods below, all of which call ensureSubDbManager()
// first, so no caller can reach useSubDb()/rebindToSubDb() while the
// catalog is still null.
//
#include "annpocketdb/api/environment.hpp"

namespace annpocketdb::api {

void Environment::ensureSubDbManager() {
#if defined(ANNPOCKETDB_WITH_SUBDATABASE)
    if (!subDbManager_) {
        subDbManager_ = std::make_unique<SubDatabaseManager>(*pm_, *tree_, options_.maxNamedDatabases);
    }
#endif
    // No-op when ANNPOCKETDB_WITH_SUBDATABASE is off -- this is never
    // actually reached in that configuration, since every method below
    // returns Status::NotSupported before calling it. Guarded anyway
    // because SubDatabaseManager's constructor (sub_database.cpp) isn't
    // compiled in that configuration, so this can't reference it there.
}

Result<SubDatabase> Environment::createSubDatabase(const std::string& name, btree::DbFlags flags,
                                                   core::SyncMode mode) {
#if !defined(ANNPOCKETDB_WITH_SUBDATABASE)
    (void)name; (void)flags; (void)mode;
    return Status::NotSupported("named sub-database support was not built (ANNPOCKETDB_WITH_SUBDATABASE)");
#else
    if (options_.readOnly) return Status::ReadOnly("environment was opened read-only");
    if (name.empty()) return Status::InvalidArgument("sub-database name empty");
    if (name.find('\0') != std::string::npos) return Status::InvalidArgument("sub-database name contains NUL");
    ensureSubDbManager();

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
#endif
}

Result<SubDatabase> Environment::openSubDatabase(const std::string& name) {
#if !defined(ANNPOCKETDB_WITH_SUBDATABASE)
    (void)name;
    return Status::NotSupported("named sub-database support was not built (ANNPOCKETDB_WITH_SUBDATABASE)");
#else
    ensureSubDbManager();
    auto h = subDbManager_->open(name);
    if (!h) return h.status();
    std::uint32_t seq = subDbManager_->currentSequence(h.value());
    return Result<SubDatabase>(SubDatabase(pm_.get(), tree_.get(), subDbManager_.get(), name, h.value(), seq));
#endif
}

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
#if !defined(ANNPOCKETDB_WITH_SUBDATABASE)
    return Status::NotSupported("named sub-database support was not built (ANNPOCKETDB_WITH_SUBDATABASE)");
#else
    ensureSubDbManager();
    auto rt = beginRead();
    if (!rt) return rt.status();
    txn::ReadTransaction rtxn = rt.moveValue();
    return subDbManager_->list(rtxn);
#endif
}

Status Environment::dropSubDatabase(const std::string& name, core::SyncMode mode) {
#if !defined(ANNPOCKETDB_WITH_SUBDATABASE)
    (void)name; (void)mode;
    return Status::NotSupported("named sub-database support was not built (ANNPOCKETDB_WITH_SUBDATABASE)");
#else
    if (options_.readOnly) return Status::ReadOnly("environment was opened read-only");
    ensureSubDbManager();

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
#endif
}

}  // namespace annpocketdb::api
