/*
 * src/capi/annpocketdb_c.cpp
 *
 * extern "C" wrapper implementation.
 *
 * This is the ONLY translation unit that includes both the public C header
 * and the private C++ headers.  It is compiled as C++20 and linked as part
 * of libannpocketdb_c (SHARED).
 *
 * Conventions:
 *   - Every function null-checks its required handle/output pointers first.
 *   - Status / Result<T> → APDB_rc via statusToRc().
 *   - Buffer lifetime for get/key/value: the result ByteBuffer is owned by
 *     the per-handle "scratch" field; its pointer is stable until the next
 *     call on that same handle.  Language bindings copy eagerly.
 */

#include "annpocketdb_c/annpocketdb.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/api/sub_database.hpp"
#include "annpocketdb/btree/cursor.hpp"
#include "annpocketdb/txn/transaction.hpp"
#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

#ifdef ANNPOCKETDB_WITH_ENCRYPTION
#  include "annpocketdb/encryption/page_cipher.hpp"
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

using namespace annpocketdb;

// Map C++ StatusCode → APDB_rc integer.
APDB_rc statusToRc(const Status& s) {
    if (s.ok()) return APDB_OK;
    switch (s.code()) {
        case StatusCode::NotFound:       return APDB_NOT_FOUND;
        case StatusCode::Corruption:     return APDB_CORRUPTION;
        case StatusCode::IOError:        return APDB_IO_ERROR;
        case StatusCode::InvalidArgument:return APDB_INVALID_ARG;
        case StatusCode::KeyExists:      return APDB_KEY_EXISTS;
        case StatusCode::TxnConflict:    return APDB_TXN_CONFLICT;
        case StatusCode::ReadOnly:       return APDB_READ_ONLY;
        case StatusCode::MapFull:        return APDB_MAP_FULL;
        case StatusCode::Incompatible:   return APDB_INCOMPATIBLE;
        case StatusCode::Busy:           return APDB_BUSY;
        case StatusCode::NotSupported:   return APDB_NOT_SUPPORTED;
        case StatusCode::Internal:       return APDB_INTERNAL;
        case StatusCode::PageNotFound:   return APDB_PAGE_NOT_FOUND;
        case StatusCode::Panic:          return APDB_PANIC;
        case StatusCode::VersionMismatch:return APDB_VERSION_MISMATCH;
        case StatusCode::InvalidFile:    return APDB_INVALID_FILE;
        case StatusCode::DbsFull:        return APDB_DBS_FULL;
        case StatusCode::ReadersFull:    return APDB_READERS_FULL;
        case StatusCode::MapResized:     return APDB_MAP_RESIZED;
        case StatusCode::BadDbi:         return APDB_BAD_DBI;
        case StatusCode::BadTxn:         return APDB_BAD_TXN;
        case StatusCode::BadValSize:     return APDB_BAD_VAL_SIZE;
        case StatusCode::Locked:         return APDB_LOCKED;
        case StatusCode::OutOfMemory:    return APDB_OUT_OF_MEMORY;
        case StatusCode::DiskFull:       return APDB_DISK_FULL;
        default:                         return APDB_INTERNAL;
    }
}

// Translate APDB_SYNC_* → core::SyncMode.
core::SyncMode syncMode(int mode) {
    switch (mode) {
        // core::SyncMode has no separate "Async" value; Flush is the
        // existing OS-buffered/MDB_MAPASYNC-equivalent mode (see
        // DurabilityFlags::mapAsync's doc comment in page_manager.hpp).
        case APDB_SYNC_ASYNC:  return core::SyncMode::Flush;
        case APDB_SYNC_NOSYNC: return core::SyncMode::NoSync;
        default:               return core::SyncMode::Durable;
    }
}

// Translate APDB_DB_* flags → btree::DbFlags.
btree::DbFlags dbFlags(uint32_t f) {
    using F = btree::DbFlags;
    F result = F::None;
    if (f & APDB_DB_REVERSE_KEY) result = result | F::ReverseKey;
    if (f & APDB_DB_DUP_SORT)    result = result | F::DupSort;
    if (f & APDB_DB_INTEGER_KEY) result = result | F::IntegerKey;
    if (f & APDB_DB_DUP_FIXED)   result = result | F::DupFixed;
    if (f & APDB_DB_INTEGER_DUP) result = result | F::IntegerDup;
    return result;
}

// Stable scratch buffer: written by get/key/value; pointer valid until next call.
struct ScratchBuf {
    ByteBuffer buf;
    void store(ByteView v) {
        buf.assign(v.begin(), v.end());
    }
    void store(const ByteBuffer& b) {
        buf = b;
    }
    const void* data() const { return buf.data(); }
    size_t      size() const { return buf.size(); }
};

} // namespace

// ---------------------------------------------------------------------------
// Internal handle structs
// ---------------------------------------------------------------------------

struct APDB_Env_s {
    api::Environment env;
    ScratchBuf       scratch;  // backing store for single-op apdb_get result
};

struct APDB_RTxn_s {
    txn::ReadTransaction txn;
    APDB_Env_s*          owner;
    ScratchBuf           scratch;
};

struct APDB_WTxn_s {
    txn::WriteTransaction txn;
    APDB_Env_s*           owner;
    ScratchBuf            scratch;
    bool                  closed = false;
};

struct APDB_Cursor_s {
    btree::Cursor cur;
    ScratchBuf    keyScratch;
    ScratchBuf    valScratch;
};

struct APDB_SubDb_s {
    api::SubDatabase subdb;
    APDB_Env_s*      owner;
    ScratchBuf       scratch;
};

struct APDB_ChangeFeedIterator_s {
    std::vector<changefeed::ChangeEvent> events;
    size_t currentIndex = 0;
};

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------

extern "C" int apdb_version_check(int major, int /*minor*/, int /*patch*/) {
    return (major == APDB_VERSION_MAJOR) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Error messages
// ---------------------------------------------------------------------------

extern "C" const char* apdb_errmsg(APDB_rc rc) {
    switch (rc) {
        case APDB_OK:               return "success";
        case APDB_NOT_FOUND:        return "key not found";
        case APDB_CORRUPTION:       return "data corruption detected";
        case APDB_IO_ERROR:         return "I/O error";
        case APDB_INVALID_ARG:      return "invalid argument";
        case APDB_KEY_EXISTS:       return "key already exists";
        case APDB_TXN_CONFLICT:     return "transaction conflict";
        case APDB_READ_ONLY:        return "transaction is read-only";
        case APDB_MAP_FULL:         return "map size limit reached";
        case APDB_INCOMPATIBLE:     return "incompatible operation";
        case APDB_BUSY:             return "resource busy";
        case APDB_NOT_SUPPORTED:    return "not supported";
        case APDB_INTERNAL:         return "internal error";
        case APDB_PAGE_NOT_FOUND:   return "requested page not found";
        case APDB_PANIC:            return "update of meta page failed";
        case APDB_VERSION_MISMATCH: return "database version mismatch";
        case APDB_INVALID_FILE:     return "not a valid database file";
        case APDB_DBS_FULL:         return "maxNamedDatabases limit reached";
        case APDB_READERS_FULL:     return "maxReaders limit reached";
        case APDB_MAP_RESIZED:      return "database map was resized by another process";
        case APDB_BAD_DBI:          return "sub-database handle changed unexpectedly";
        case APDB_BAD_TXN:          return "transaction cannot recover; abort it";
        case APDB_BAD_VAL_SIZE:     return "key/value too large or size mismatch";
        case APDB_LOCKED:           return "environment locked by another process";
        case APDB_OUT_OF_MEMORY:    return "out of memory";
        case APDB_DISK_FULL:        return "no more disk space";
        default:                    return "unknown error";
    }
}

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------

extern "C" APDB_rc apdb_env_open(const char* path,
                                  const APDB_EnvOptions* opts,
                                  APDB_Env* env_out) {
    if (!path || !env_out) return APDB_INVALID_ARG;

    api::EnvironmentOptions cpp_opts;
    if (opts) {
        if (opts->flags & APDB_ENV_RDONLY)       cpp_opts.readOnly       = true;
        if (opts->flags & APDB_ENV_NO_SYNC)      cpp_opts.noSync         = true;
        if (opts->flags & APDB_ENV_NO_META_SYNC) cpp_opts.noMetaSync     = true;
        if (opts->flags & APDB_ENV_MAP_ASYNC)    cpp_opts.mapAsync        = true;
        if (opts->flags & APDB_ENV_NO_LOCK)      cpp_opts.multiProcess    = false;
        if (opts->flags & APDB_ENV_WAL)          cpp_opts.enableWal       = true;
        if (opts->flags & APDB_ENV_COMPRESSION)  cpp_opts.enableCompression = true;
        if (opts->flags & APDB_ENV_DURABLE_CRC)  cpp_opts.durableCrc      = true;
        if (opts->flags & APDB_ENV_CHANGEFEED)       cpp_opts.enableChangefeed = true;
        if (opts->flags & APDB_ENV_CHANGEFEED_ASYNC) cpp_opts.changefeedAsync = true;
        if (opts->flags & APDB_ENV_ENCRYPTION) {
            cpp_opts.enableEncryption = true;
            if (opts->encryption_key) {
                std::memcpy(cpp_opts.encryptionKey.data(),
                            opts->encryption_key, 32);
            }
        }
        if (opts->page_size)        cpp_opts.pageSize           = opts->page_size;
        if (opts->map_size)         cpp_opts.maxDatabaseSize    = opts->map_size;
        if (opts->initial_file_size)cpp_opts.initialFileSize    = opts->initial_file_size;
        if (opts->max_readers)      cpp_opts.maxReaders         = opts->max_readers;
        if (opts->max_named_dbs)    cpp_opts.maxNamedDatabases  = opts->max_named_dbs;
        if (opts->compression_min_size)
            cpp_opts.compressionMinSize = opts->compression_min_size;
    }

    auto result = api::Environment::open(path, cpp_opts);
    if (!result) return statusToRc(result.status());

    auto* h = new (std::nothrow) APDB_Env_s{std::move(result.value()), {}};
    if (!h) return APDB_OUT_OF_MEMORY;
    *env_out = h;
    return APDB_OK;
}

extern "C" APDB_rc apdb_env_close(APDB_Env env) {
    if (!env) return APDB_INVALID_ARG;
    auto s = env->env.close();
    delete env;
    return statusToRc(s);
}

extern "C" APDB_rc apdb_env_sync(APDB_Env env, int force) {
    if (!env) return APDB_INVALID_ARG;
    return statusToRc(env->env.sync(force != 0));
}

extern "C" APDB_rc apdb_env_set_map_size(APDB_Env env, uint64_t new_max_bytes) {
    if (!env) return APDB_INVALID_ARG;
    return statusToRc(env->env.setMapSize(new_max_bytes));
}

extern "C" uint64_t apdb_env_map_size(APDB_Env env) {
    if (!env) return 0;
    return env->env.mapSize();
}

extern "C" size_t apdb_env_max_key_size(APDB_Env env) {
    if (!env) return 0;
    return env->env.maxKeySize();
}

extern "C" size_t apdb_env_reader_check(APDB_Env env) {
    if (!env) return 0;
    return env->env.readerCheck();
}

extern "C" APDB_rc apdb_env_stat(APDB_Env env, APDB_Stat* stat_out) {
    if (!env || !stat_out) return APDB_INVALID_ARG;
    auto r = env->env.stat();
    if (!r) return statusToRc(r.status());
    const auto& s        = r.value();
    stat_out->page_size   = s.pageSize;
    stat_out->depth       = s.depth;
    stat_out->branch_pages= s.branchPages;
    stat_out->leaf_pages  = s.leafPages;
    stat_out->overflow_pages = s.overflowPages;
    stat_out->entries     = s.entries;
    return APDB_OK;
}

extern "C" void apdb_env_info(APDB_Env env, APDB_EnvInfo* info_out) {
    if (!env || !info_out) return;
    auto info          = env->env.info();
    info_out->map_size  = info.mapSize;
    info_out->last_page = info.lastPageId;
    info_out->last_txn_id   = info.lastTxnId;
    info_out->max_readers   = info.maxReaders;
    info_out->num_readers   = info.numReaders;
}

// ---------------------------------------------------------------------------
// Default-keyspace single-op helpers
// ---------------------------------------------------------------------------

extern "C" APDB_rc apdb_get(APDB_Env env,
                             const void* key, size_t key_len,
                             const void** data_out, size_t* size_out) {
    if (!env || !key || !data_out || !size_out) return APDB_INVALID_ARG;
    ByteView k(static_cast<const std::byte*>(key), key_len);
    auto r = env->env.get(k);
    if (!r) return statusToRc(r.status());
    env->scratch.store(r.value());
    *data_out = env->scratch.data();
    *size_out = env->scratch.size();
    return APDB_OK;
}

extern "C" APDB_rc apdb_put(APDB_Env env,
                             const void* key, size_t key_len,
                             const void* val, size_t val_len) {
    if (!env || !key || !val) return APDB_INVALID_ARG;
    ByteView k(static_cast<const std::byte*>(key), key_len);
    ByteView v(static_cast<const std::byte*>(val), val_len);
    return statusToRc(env->env.put(k, v, /*overwrite=*/true));
}

extern "C" APDB_rc apdb_put_nooverwrite(APDB_Env env,
                                         const void* key, size_t key_len,
                                         const void* val, size_t val_len) {
    if (!env || !key || !val) return APDB_INVALID_ARG;
    ByteView k(static_cast<const std::byte*>(key), key_len);
    ByteView v(static_cast<const std::byte*>(val), val_len);
    return statusToRc(env->env.put(k, v, /*overwrite=*/false));
}

extern "C" APDB_rc apdb_del(APDB_Env env,
                             const void* key, size_t key_len) {
    if (!env || !key) return APDB_INVALID_ARG;
    ByteView k(static_cast<const std::byte*>(key), key_len);
    return statusToRc(env->env.remove(k));
}

// ---------------------------------------------------------------------------
// Read transactions
// ---------------------------------------------------------------------------

extern "C" APDB_rc apdb_txn_begin_read(APDB_Env env, APDB_RTxn* txn_out) {
    if (!env || !txn_out) return APDB_INVALID_ARG;
    auto r = env->env.beginRead();
    if (!r) return statusToRc(r.status());
    auto* h = new (std::nothrow) APDB_RTxn_s{std::move(r.value()), env, {}};
    if (!h) return APDB_OUT_OF_MEMORY;
    *txn_out = h;
    return APDB_OK;
}

extern "C" APDB_rc apdb_rtxn_get(APDB_RTxn txn,
                                   const void* key, size_t key_len,
                                   const void** data_out, size_t* size_out) {
    if (!txn || !key || !data_out || !size_out) return APDB_INVALID_ARG;
    ByteView k(static_cast<const std::byte*>(key), key_len);
    auto r = txn->txn.get(k);
    if (!r) return statusToRc(r.status());
    txn->scratch.store(r.value());
    *data_out = txn->scratch.data();
    *size_out = txn->scratch.size();
    return APDB_OK;
}

extern "C" APDB_rc apdb_rtxn_contains(APDB_RTxn txn,
                                       const void* key, size_t key_len,
                                       int* exists_out) {
    if (!txn || !key || !exists_out) return APDB_INVALID_ARG;
    ByteView k(static_cast<const std::byte*>(key), key_len);
    auto r = txn->txn.contains(k);
    if (!r) return statusToRc(r.status());
    *exists_out = r.value() ? 1 : 0;
    return APDB_OK;
}

extern "C" uint64_t apdb_rtxn_id(APDB_RTxn txn) {
    if (!txn) return 0;
    return txn->txn.txnId();
}

extern "C" APDB_rc apdb_rtxn_reset(APDB_RTxn txn) {
    if (!txn) return APDB_INVALID_ARG;
    return statusToRc(txn->txn.reset());
}

extern "C" APDB_rc apdb_rtxn_renew(APDB_RTxn txn) {
    if (!txn) return APDB_INVALID_ARG;
    return statusToRc(txn->txn.renew());
}

extern "C" APDB_rc apdb_rtxn_close(APDB_RTxn txn) {
    if (!txn) return APDB_INVALID_ARG;
    delete txn;
    return APDB_OK;
}

// ---------------------------------------------------------------------------
// Write transactions
// ---------------------------------------------------------------------------

extern "C" APDB_rc apdb_txn_begin_write(APDB_Env env, APDB_WTxn* txn_out) {
    if (!env || !txn_out) return APDB_INVALID_ARG;
    auto r = env->env.beginWrite();
    if (!r) return statusToRc(r.status());
    auto* h = new (std::nothrow) APDB_WTxn_s{std::move(r.value()), env, {}, false};
    if (!h) return APDB_OUT_OF_MEMORY;
    *txn_out = h;
    return APDB_OK;
}

extern "C" APDB_rc apdb_wtxn_put(APDB_WTxn txn,
                                   const void* key, size_t key_len,
                                   const void* val, size_t val_len,
                                   int overwrite) {
    if (!txn || !key || !val) return APDB_INVALID_ARG;
    ByteView k(static_cast<const std::byte*>(key), key_len);
    ByteView v(static_cast<const std::byte*>(val), val_len);
    return statusToRc(txn->txn.put(k, v, overwrite != 0));
}

extern "C" APDB_rc apdb_wtxn_del(APDB_WTxn txn,
                                   const void* key, size_t key_len) {
    if (!txn || !key) return APDB_INVALID_ARG;
    ByteView k(static_cast<const std::byte*>(key), key_len);
    return statusToRc(txn->txn.remove(k));
}

extern "C" APDB_rc apdb_wtxn_get(APDB_WTxn txn,
                                   const void* key, size_t key_len,
                                   const void** data_out, size_t* size_out) {
    if (!txn || !key || !data_out || !size_out) return APDB_INVALID_ARG;
    ByteView k(static_cast<const std::byte*>(key), key_len);
    auto r = txn->txn.get(k);
    if (!r) return statusToRc(r.status());
    txn->scratch.store(r.value());
    *data_out = txn->scratch.data();
    *size_out = txn->scratch.size();
    return APDB_OK;
}

extern "C" APDB_rc apdb_wtxn_contains(APDB_WTxn txn,
                                       const void* key, size_t key_len,
                                       int* exists_out) {
    if (!txn || !key || !exists_out) return APDB_INVALID_ARG;
    ByteView k(static_cast<const std::byte*>(key), key_len);
    auto r = txn->txn.contains(k);
    if (!r) return statusToRc(r.status());
    *exists_out = r.value() ? 1 : 0;
    return APDB_OK;
}

extern "C" uint64_t apdb_wtxn_id(APDB_WTxn txn) {
    if (!txn) return 0;
    return txn->txn.txnId();
}

extern "C" APDB_rc apdb_wtxn_commit(APDB_WTxn txn, int sync_mode) {
    if (!txn) return APDB_INVALID_ARG;
    if (txn->closed) return APDB_BAD_TXN;
    auto s = txn->txn.commit(syncMode(sync_mode));
    txn->closed = true;
    APDB_rc rc = statusToRc(s);
    delete txn;
    return rc;
}

extern "C" APDB_rc apdb_wtxn_abort(APDB_WTxn txn) {
    if (!txn) return APDB_INVALID_ARG;
    if (txn->closed) return APDB_OK;
    auto s = txn->txn.abort();
    txn->closed = true;
    APDB_rc rc = statusToRc(s);
    delete txn;
    return rc;
}

extern "C" APDB_rc apdb_wtxn_close(APDB_WTxn txn) {
    if (!txn) return APDB_INVALID_ARG;
    if (!txn->closed) {
        txn->txn.abort();
    }
    delete txn;
    return APDB_OK;
}

// ---------------------------------------------------------------------------
// 2PC & Blob Storage
// ---------------------------------------------------------------------------

extern "C" APDB_rc apdb_wtxn_prepare(APDB_WTxn txn, int sync_mode) {
    if (!txn) return APDB_INVALID_ARG;
    if (txn->closed) return APDB_BAD_TXN;
    return statusToRc(txn->txn.prepare(syncMode(sync_mode)));
}

extern "C" APDB_rc apdb_env_rollback_prepared(APDB_Env env) {
    if (!env) return APDB_INVALID_ARG;
    return statusToRc(env->env.rollbackPrepared());
}

extern "C" APDB_rc apdb_env_put_blob(APDB_Env env,
                                     const void* key, size_t key_len,
                                     const void* blob, size_t blob_len,
                                     int sync_mode) {
    if (!env || !key || !blob) return APDB_INVALID_ARG;
    ByteView k(static_cast<const std::byte*>(key), key_len);
    ByteView b(static_cast<const std::byte*>(blob), blob_len);
    auto r = env->env.putBlob(k, b, syncMode(sync_mode));
    return statusToRc(r.status());
}

extern "C" APDB_rc apdb_env_get_blob(APDB_Env env,
                                     const void* key, size_t key_len,
                                     const void** data_out, size_t* size_out) {
    if (!env || !key || !data_out || !size_out) return APDB_INVALID_ARG;
    ByteView k(static_cast<const std::byte*>(key), key_len);
    auto r = env->env.getBlob(k);
    if (!r) return statusToRc(r.status());
    env->scratch.store(r.value());
    *data_out = env->scratch.data();
    *size_out = env->scratch.size();
    return APDB_OK;
}

extern "C" APDB_rc apdb_env_del_blob(APDB_Env env, const void* key, size_t key_len, int sync_mode) {
    if (!env || !key) return APDB_INVALID_ARG;
    ByteView k(static_cast<const std::byte*>(key), key_len);
    return statusToRc(env->env.removeBlob(k, syncMode(sync_mode)));
}

// ---------------------------------------------------------------------------
// Cursors — shared helpers
// ---------------------------------------------------------------------------

namespace {

APDB_Cursor make_cursor(btree::Cursor c) {
    auto* h = new (std::nothrow) APDB_Cursor_s{std::move(c), {}, {}};
    return h;
}

} // namespace

extern "C" APDB_rc apdb_rtxn_cursor_open(APDB_RTxn txn, APDB_Cursor* cur_out) {
    if (!txn || !cur_out) return APDB_INVALID_ARG;
    auto c = txn->txn.openCursor();
    *cur_out = make_cursor(std::move(c));
    if (!*cur_out) return APDB_OUT_OF_MEMORY;
    return APDB_OK;
}

extern "C" APDB_rc apdb_wtxn_cursor_open(APDB_WTxn txn, APDB_Cursor* cur_out) {
    if (!txn || !cur_out) return APDB_INVALID_ARG;
    auto c = txn->txn.openCursor();
    *cur_out = make_cursor(std::move(c));
    if (!*cur_out) return APDB_OUT_OF_MEMORY;
    return APDB_OK;
}

extern "C" APDB_rc apdb_cursor_first(APDB_Cursor cur) {
    if (!cur) return APDB_INVALID_ARG;
    auto r = cur->cur.first();
    if (!r) return statusToRc(r.status());
    return r.value() ? APDB_OK : APDB_NOT_FOUND;
}

extern "C" APDB_rc apdb_cursor_last(APDB_Cursor cur) {
    if (!cur) return APDB_INVALID_ARG;
    auto r = cur->cur.last();
    if (!r) return statusToRc(r.status());
    return r.value() ? APDB_OK : APDB_NOT_FOUND;
}

extern "C" APDB_rc apdb_cursor_next(APDB_Cursor cur) {
    if (!cur) return APDB_INVALID_ARG;
    auto r = cur->cur.next();
    if (!r) return statusToRc(r.status());
    return r.value() ? APDB_OK : APDB_NOT_FOUND;
}

extern "C" APDB_rc apdb_cursor_prev(APDB_Cursor cur) {
    if (!cur) return APDB_INVALID_ARG;
    auto r = cur->cur.prev();
    if (!r) return statusToRc(r.status());
    return r.value() ? APDB_OK : APDB_NOT_FOUND;
}

extern "C" APDB_rc apdb_cursor_seek(APDB_Cursor cur,
                                     const void* key, size_t key_len) {
    if (!cur || !key) return APDB_INVALID_ARG;
    ByteView k(static_cast<const std::byte*>(key), key_len);
    auto r = cur->cur.seek(k);
    if (!r) return statusToRc(r.status());
    return r.value() ? APDB_OK : APDB_NOT_FOUND;
}

extern "C" APDB_rc apdb_cursor_seek_exact(APDB_Cursor cur,
                                           const void* key, size_t key_len) {
    if (!cur || !key) return APDB_INVALID_ARG;
    ByteView k(static_cast<const std::byte*>(key), key_len);
    auto r = cur->cur.seekExact(k);
    if (!r) return statusToRc(r.status());
    return r.value() ? APDB_OK : APDB_NOT_FOUND;
}

extern "C" int apdb_cursor_valid(APDB_Cursor cur) {
    if (!cur) return 0;
    return cur->cur.valid() ? 1 : 0;
}

extern "C" APDB_rc apdb_cursor_key(APDB_Cursor cur,
                                    const void** key_out, size_t* key_len_out) {
    if (!cur || !key_out || !key_len_out) return APDB_INVALID_ARG;
    auto r = cur->cur.key();
    if (!r) return statusToRc(r.status());
    cur->keyScratch.store(r.value());
    *key_out     = cur->keyScratch.data();
    *key_len_out = cur->keyScratch.size();
    return APDB_OK;
}

extern "C" APDB_rc apdb_cursor_value(APDB_Cursor cur,
                                      const void** val_out, size_t* val_len_out) {
    if (!cur || !val_out || !val_len_out) return APDB_INVALID_ARG;
    auto r = cur->cur.value();
    if (!r) return statusToRc(r.status());
    cur->valScratch.store(r.value());
    *val_out     = cur->valScratch.data();
    *val_len_out = cur->valScratch.size();
    return APDB_OK;
}

extern "C" APDB_rc apdb_cursor_put(APDB_Cursor cur,
                                    const void* key, size_t key_len,
                                    const void* val, size_t val_len,
                                    uint32_t flags) {
    if (!cur || !key || !val) return APDB_INVALID_ARG;
    ByteView k(static_cast<const std::byte*>(key), key_len);
    ByteView v(static_cast<const std::byte*>(val), val_len);
    using F = btree::CursorPutFlags;
    F cpp_flags = F::None;
    if (flags & APDB_CURSOR_NO_OVERWRITE) cpp_flags = cpp_flags | F::NoOverwrite;
    if (flags & APDB_CURSOR_APPEND)       cpp_flags = cpp_flags | F::Append;
    if (flags & APDB_CURSOR_CURRENT)      cpp_flags = cpp_flags | F::Current;
    return statusToRc(cur->cur.put(k, v, cpp_flags));
}

extern "C" APDB_rc apdb_cursor_del(APDB_Cursor cur) {
    if (!cur) return APDB_INVALID_ARG;
    return statusToRc(cur->cur.del());
}

extern "C" APDB_rc apdb_cursor_close(APDB_Cursor cur) {
    if (!cur) return APDB_INVALID_ARG;
    delete cur;
    return APDB_OK;
}

// ---------------------------------------------------------------------------
// Named sub-databases
// ---------------------------------------------------------------------------

extern "C" APDB_rc apdb_subdb_create(APDB_Env env, const char* name,
                                      uint32_t flags, APDB_SubDb* subdb_out) {
    if (!env || !name || !subdb_out) return APDB_INVALID_ARG;
    auto r = env->env.createSubDatabase(name, dbFlags(flags));
    if (!r) return statusToRc(r.status());
    auto* h = new (std::nothrow) APDB_SubDb_s{std::move(r.value()), env, {}};
    if (!h) return APDB_OUT_OF_MEMORY;
    *subdb_out = h;
    return APDB_OK;
}

extern "C" APDB_rc apdb_subdb_open(APDB_Env env, const char* name,
                                    APDB_SubDb* subdb_out) {
    if (!env || !name || !subdb_out) return APDB_INVALID_ARG;
    auto r = env->env.openSubDatabase(name);
    if (!r) return statusToRc(r.status());
    auto* h = new (std::nothrow) APDB_SubDb_s{std::move(r.value()), env, {}};
    if (!h) return APDB_OUT_OF_MEMORY;
    *subdb_out = h;
    return APDB_OK;
}

extern "C" APDB_rc apdb_subdb_open_or_create(APDB_Env env, const char* name,
                                              uint32_t flags, APDB_SubDb* subdb_out) {
    if (!env || !name || !subdb_out) return APDB_INVALID_ARG;
    auto r = env->env.openOrCreateSubDatabase(name, dbFlags(flags));
    if (!r) return statusToRc(r.status());
    auto* h = new (std::nothrow) APDB_SubDb_s{std::move(r.value()), env, {}};
    if (!h) return APDB_OUT_OF_MEMORY;
    *subdb_out = h;
    return APDB_OK;
}

extern "C" APDB_rc apdb_subdb_list(APDB_Env env,
                                    char*** names_out, size_t* count_out) {
    if (!env || !names_out || !count_out) return APDB_INVALID_ARG;
    auto r = env->env.listSubDatabases();
    if (!r) return statusToRc(r.status());
    const auto& names = r.value();
    *count_out = names.size();
    if (names.empty()) { *names_out = nullptr; return APDB_OK; }
    auto** arr = static_cast<char**>(std::malloc(names.size() * sizeof(char*)));
    if (!arr) return APDB_OUT_OF_MEMORY;
    for (std::size_t i = 0; i < names.size(); ++i) {
        arr[i] = static_cast<char*>(std::malloc(names[i].size() + 1));
        if (!arr[i]) {
            for (std::size_t j = 0; j < i; ++j) std::free(arr[j]);
            std::free(arr);
            return APDB_OUT_OF_MEMORY;
        }
        std::memcpy(arr[i], names[i].c_str(), names[i].size() + 1);
    }
    *names_out = arr;
    return APDB_OK;
}

extern "C" APDB_rc apdb_subdb_drop(APDB_Env env, const char* name, int smode) {
    if (!env || !name) return APDB_INVALID_ARG;
    return statusToRc(env->env.dropSubDatabase(name, syncMode(smode)));
}

extern "C" void apdb_subdb_close(APDB_SubDb subdb) {
    if (!subdb) return;
    subdb->subdb.close();
    delete subdb;
}

extern "C" APDB_rc apdb_subdb_stat(APDB_SubDb subdb, APDB_Stat* stat_out) {
    if (!subdb || !stat_out) return APDB_INVALID_ARG;
    // stat via the sub-database's root page id through a read transaction.
    auto r = subdb->owner->env.stat();
    if (!r) return statusToRc(r.status());
    const auto& s        = r.value();
    stat_out->page_size   = s.pageSize;
    stat_out->depth       = s.depth;
    stat_out->branch_pages= s.branchPages;
    stat_out->leaf_pages  = s.leafPages;
    stat_out->overflow_pages = s.overflowPages;
    stat_out->entries     = s.entries;
    return APDB_OK;
}

extern "C" APDB_rc apdb_subdb_get(APDB_SubDb subdb,
                                   const void* key, size_t key_len,
                                   const void** data_out, size_t* size_out) {
    if (!subdb || !key || !data_out || !size_out) return APDB_INVALID_ARG;
    ByteView k(static_cast<const std::byte*>(key), key_len);
    auto r = subdb->subdb.get(k);
    if (!r) return statusToRc(r.status());
    subdb->scratch.store(r.value());
    *data_out = subdb->scratch.data();
    *size_out = subdb->scratch.size();
    return APDB_OK;
}

extern "C" APDB_rc apdb_subdb_put(APDB_SubDb subdb,
                                   const void* key, size_t key_len,
                                   const void* val, size_t val_len,
                                   int overwrite, int smode) {
    if (!subdb || !key || !val) return APDB_INVALID_ARG;
    ByteView k(static_cast<const std::byte*>(key), key_len);
    ByteView v(static_cast<const std::byte*>(val), val_len);
    return statusToRc(subdb->subdb.put(k, v, overwrite != 0, syncMode(smode)));
}

extern "C" APDB_rc apdb_subdb_del(APDB_SubDb subdb,
                                   const void* key, size_t key_len,
                                   int smode) {
    if (!subdb || !key) return APDB_INVALID_ARG;
    ByteView k(static_cast<const std::byte*>(key), key_len);
    return statusToRc(subdb->subdb.remove(k, syncMode(smode)));
}

extern "C" APDB_rc apdb_subdb_begin_write(APDB_SubDb subdb, APDB_WTxn* txn_out) {
    if (!subdb || !txn_out) return APDB_INVALID_ARG;
    auto r = subdb->subdb.beginWrite();
    if (!r) return statusToRc(r.status());
    auto* h = new (std::nothrow) APDB_WTxn_s{std::move(r.value()), subdb->owner, {}, false};
    if (!h) return APDB_OUT_OF_MEMORY;
    *txn_out = h;
    return APDB_OK;
}

extern "C" APDB_rc apdb_subdb_begin_read(APDB_SubDb subdb, APDB_RTxn* txn_out) {
    if (!subdb || !txn_out) return APDB_INVALID_ARG;
    auto r = subdb->subdb.beginRead();
    if (!r) return statusToRc(r.status());
    auto* h = new (std::nothrow) APDB_RTxn_s{std::move(r.value()), subdb->owner, {}};
    if (!h) return APDB_OUT_OF_MEMORY;
    *txn_out = h;
    return APDB_OK;
}

extern "C" APDB_rc apdb_subdb_cursor_open_write(APDB_SubDb subdb,
                                                 APDB_WTxn txn,
                                                 APDB_Cursor* cur_out) {
    if (!subdb || !txn || !cur_out) return APDB_INVALID_ARG;
    auto c = txn->txn.openCursor();
    *cur_out = make_cursor(std::move(c));
    if (!*cur_out) return APDB_OUT_OF_MEMORY;
    return APDB_OK;
}

extern "C" APDB_rc apdb_subdb_cursor_open_read(APDB_SubDb subdb,
                                                APDB_RTxn txn,
                                                APDB_Cursor* cur_out) {
    if (!subdb || !txn || !cur_out) return APDB_INVALID_ARG;
    auto c = txn->txn.openCursor();
    *cur_out = make_cursor(std::move(c));
    if (!*cur_out) return APDB_OUT_OF_MEMORY;
    return APDB_OK;
}

// ---------------------------------------------------------------------------
// Changefeed
// ---------------------------------------------------------------------------

extern "C" APDB_rc apdb_changefeed_subscribe(APDB_Env env, APDB_ChangeCallback cb, void* user_ctx) {
    if (!env || !cb) return APDB_INVALID_ARG;
    auto* cf = env->env.changeFeed();
    if (!cf) return APDB_NOT_SUPPORTED;
    cf->subscribe([cb, user_ctx](const changefeed::ChangeEvent& e) {
        APDB_ChangeEvent c_event;
        c_event.sequence = e.sequence;
        c_event.type = static_cast<uint8_t>(e.type);
        c_event.txn_id = e.txnId;
        c_event.key = e.key.data();
        c_event.key_len = e.key.size();
        c_event.old_value = e.oldValue.data();
        c_event.old_value_len = e.oldValue.size();
        c_event.new_value = e.newValue.data();
        c_event.new_value_len = e.newValue.size();
        cb(&c_event, user_ctx);
    });
    return APDB_OK;
}

extern "C" APDB_rc apdb_changefeed_unsubscribe(APDB_Env env) {
    if (!env) return APDB_INVALID_ARG;
    auto* cf = env->env.changeFeed();
    if (!cf) return APDB_NOT_SUPPORTED;
    cf->unsubscribe();
    return APDB_OK;
}

extern "C" APDB_rc apdb_changefeed_open_poll(APDB_Env env, APDB_RTxn txn,
                                             uint64_t after_token,
                                             APDB_ChangeFeedIterator* iter_out) {
    if (!env || !txn || !iter_out) return APDB_INVALID_ARG;
    auto* cf = env->env.changeFeed();
    if (!cf) return APDB_NOT_SUPPORTED;
    auto r = cf->pollFrom(txn->txn, after_token, 0);
    if (!r) return statusToRc(r.status());
    auto* iter = new (std::nothrow) APDB_ChangeFeedIterator_s{std::move(r.value()), 0};
    if (!iter) return APDB_OUT_OF_MEMORY;
    *iter_out = iter;
    return APDB_OK;
}

extern "C" APDB_rc apdb_changefeed_next(APDB_ChangeFeedIterator iter, APDB_ChangeEvent* event_out) {
    if (!iter || !event_out) return APDB_INVALID_ARG;
    if (iter->currentIndex >= iter->events.size()) return APDB_NOT_FOUND;
    const auto& e = iter->events[iter->currentIndex++];
    event_out->sequence = e.sequence;
    event_out->type = static_cast<uint8_t>(e.type);
    event_out->txn_id = e.txnId;
    event_out->key = e.key.data();
    event_out->key_len = e.key.size();
    event_out->old_value = e.oldValue.data();
    event_out->old_value_len = e.oldValue.size();
    event_out->new_value = e.newValue.data();
    event_out->new_value_len = e.newValue.size();
    return APDB_OK;
}

extern "C" APDB_rc apdb_changefeed_close_poll(APDB_ChangeFeedIterator iter) {
    if (!iter) return APDB_INVALID_ARG;
    delete iter;
    return APDB_OK;
}

// ---------------------------------------------------------------------------
// Owned byte buffer
// ---------------------------------------------------------------------------

extern "C" APDB_rc apdb_bytes_copy(const void* src, size_t size, APDB_Bytes* out) {
    if (!src || !out) return APDB_INVALID_ARG;
    out->data = std::malloc(size);
    if (!out->data) return APDB_OUT_OF_MEMORY;
    std::memcpy(out->data, src, size);
    out->size = size;
    return APDB_OK;
}

extern "C" void apdb_bytes_free(APDB_Bytes* b) {
    if (b && b->data) {
        std::free(b->data);
        b->data = nullptr;
        b->size = 0;
    }
}

// ---------------------------------------------------------------------------
// Key derivation (encryption build only)
// ---------------------------------------------------------------------------

#ifdef ANNPOCKETDB_WITH_ENCRYPTION
extern "C" APDB_rc apdb_derive_key(const char* passphrase, size_t passphrase_len,
                                    const uint8_t* salt, size_t salt_len,
                                    uint8_t key_out[32]) {
    if (!passphrase || !salt || !key_out) return APDB_INVALID_ARG;
    // deriveKeyFromPassphrase takes a std::string_view (not a byte view) for
    // the passphrase, and writes into an actual encryption::Key
    // (std::array<uint8_t,32>&, not a {pointer,size} view) -- derive into a
    // local then copy out, since key_out is a raw C array.
    encryption::Key derived{};
    Status s = encryption::deriveKeyFromPassphrase(
        std::string_view(passphrase, passphrase_len),
        ByteView(reinterpret_cast<const std::byte*>(salt), salt_len),
        derived);
    if (s.ok()) std::memcpy(key_out, derived.data(), derived.size());
    return statusToRc(s);
}
#endif
