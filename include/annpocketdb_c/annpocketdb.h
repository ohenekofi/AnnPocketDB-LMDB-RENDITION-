/*
 * annpocketdb_c/annpocketdb.h
 *
 * Stable, pure-C99 public interface for AnnPocketDb.
 *
 * Design contract (matches LMDB's own FFI conventions):
 *
 *   HANDLES     Opaque pointers. Allocate via apdb_*_open/begin functions;
 *               free via the matching apdb_*_close/commit/abort.
 *
 *   ERROR MODEL Every function returns APDB_rc (int).  0 == APDB_OK.
 *               apdb_errmsg(rc) returns a static string — no allocation.
 *               Functions that produce a value also take (out*) parameters.
 *
 *   BUFFER LIFETIME  apdb_*_get / apdb_cursor_key / apdb_cursor_value write
 *               a pointer + size into (*data_out, *size_out).  That pointer
 *               is valid until the NEXT call on the same transaction or
 *               cursor handle, matching LMDB's MDB_val lifetime rule.
 *               Language bindings copy eagerly into their own managed heap.
 *
 *   THREAD SAFETY  Same rules as LMDB:
 *               - APDB_Env may be shared across threads (internal locking).
 *               - APDB_RTxn is move-safe but NOT concurrently usable.
 *               - APDB_WTxn must be driven from the thread that opened it.
 *               - APDB_Cursor must not outlive its owning transaction.
 *
 * Versioning: APDB_VERSION_MAJOR/MINOR/PATCH follow semantic versioning.
 * ABI stability: once a symbol is published in a MAJOR version it is never
 * removed or changed; new symbols are only added.
 */
#ifndef ANNPOCKETDB_C_H
#define ANNPOCKETDB_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Version
 * -------------------------------------------------------------------------- */
#define APDB_VERSION_MAJOR 0
#define APDB_VERSION_MINOR 1
#define APDB_VERSION_PATCH 0

/** Runtime version check — returns 0 if the loaded library matches these
 *  macros, non-zero if there is a major-version mismatch. */
int apdb_version_check(int major, int minor, int patch);

/* --------------------------------------------------------------------------
 * Opaque handle types
 * -------------------------------------------------------------------------- */
typedef struct APDB_Env_s    *APDB_Env;
typedef struct APDB_RTxn_s   *APDB_RTxn;
typedef struct APDB_WTxn_s   *APDB_WTxn;
typedef struct APDB_Cursor_s *APDB_Cursor;
typedef struct APDB_SubDb_s  *APDB_SubDb;
typedef struct APDB_ChangeFeedIterator_s *APDB_ChangeFeedIterator;

/* --------------------------------------------------------------------------
 * Return codes  (mirrors annpocketdb::StatusCode 1:1)
 * -------------------------------------------------------------------------- */
typedef int APDB_rc;

#define APDB_OK               0
#define APDB_NOT_FOUND        1
#define APDB_CORRUPTION       2
#define APDB_IO_ERROR         3
#define APDB_INVALID_ARG      4
#define APDB_KEY_EXISTS       5
#define APDB_TXN_CONFLICT     6
#define APDB_READ_ONLY        7
#define APDB_MAP_FULL         8
#define APDB_INCOMPATIBLE     9
#define APDB_BUSY            10
#define APDB_NOT_SUPPORTED   11
#define APDB_INTERNAL        12
/* LMDB-parity granular codes */
#define APDB_PAGE_NOT_FOUND  13
#define APDB_PANIC           14
#define APDB_VERSION_MISMATCH 15
#define APDB_INVALID_FILE    16
#define APDB_DBS_FULL        17
#define APDB_READERS_FULL    18
#define APDB_MAP_RESIZED     19
#define APDB_BAD_DBI         20
#define APDB_BAD_TXN         21
#define APDB_BAD_VAL_SIZE    22
#define APDB_LOCKED          23
#define APDB_OUT_OF_MEMORY   24
#define APDB_DISK_FULL       25

/** Static string description of a return code.  Never NULL. */
const char *apdb_errmsg(APDB_rc rc);

/* --------------------------------------------------------------------------
 * Environment option flags  (bitmask for apdb_env_open `flags` parameter)
 * -------------------------------------------------------------------------- */
#define APDB_ENV_RDONLY       (1u << 0)  /**< Open read-only                */
#define APDB_ENV_NO_SYNC      (1u << 1)  /**< MDB_NOSYNC                    */
#define APDB_ENV_NO_META_SYNC (1u << 2)  /**< MDB_NOMETASYNC                */
#define APDB_ENV_MAP_ASYNC    (1u << 3)  /**< MDB_MAPASYNC                  */
#define APDB_ENV_NO_LOCK      (1u << 4)  /**< Single-process, no .lock file */
#define APDB_ENV_WAL          (1u << 5)  /**< Enable write-ahead log        */
#define APDB_ENV_COMPRESSION  (1u << 6)  /**< Enable LZ4 value compression  */
#define APDB_ENV_ENCRYPTION   (1u << 7)  /**< Enable page-level encryption  */
#define APDB_ENV_DURABLE_CRC  (1u << 8)  /**< Enable per-page CRC32C        */
#define APDB_ENV_CHANGEFEED       (1u << 9)  /**< Enable durable changefeed     */
#define APDB_ENV_CHANGEFEED_ASYNC (1u << 10) /**< Async changefeed delivery     */

/** Full environment open options (pass NULL for defaults). */
typedef struct APDB_EnvOptions {
    uint32_t flags;              /**< APDB_ENV_* bitmask                    */
    uint32_t page_size;          /**< 0 = default (4096)                    */
    uint64_t map_size;           /**< Virtual address ceiling; 0 = 1 TiB    */
    uint64_t initial_file_size;  /**< 0 = page_size * 16                    */
    uint32_t max_readers;        /**< 0 = 256                               */
    uint32_t max_named_dbs;      /**< 0 = unlimited                         */
    /**
     * Encryption key (32 bytes, XChaCha20-Poly1305).
     * Only used when APDB_ENV_ENCRYPTION is set.
     * The caller owns key derivation / storage (use apdb_derive_key).
     */
    const uint8_t *encryption_key; /**< NULL = no encryption key           */
    uint32_t compression_min_size; /**< 0 = 256 bytes                      */
} APDB_EnvOptions;

/* --------------------------------------------------------------------------
 * Environment
 * -------------------------------------------------------------------------- */

/**
 * Open or create a database environment at `path`.
 *
 * @param path     Filesystem path to the database directory / file.
 * @param opts     Options (NULL = all defaults).
 * @param env_out  Receives the new environment handle on APDB_OK.
 * @return APDB_OK on success, error code otherwise.
 */
APDB_rc apdb_env_open(const char *path, const APDB_EnvOptions *opts,
                      APDB_Env *env_out);

/**
 * Close the environment and free all associated resources.
 * All transactions and cursors must be closed first.
 * `env` is invalid after this call.
 */
APDB_rc apdb_env_close(APDB_Env env);

/**
 * Flush outstanding OS buffers to disk.
 * @param force  Non-zero: flush even when NO_SYNC / MAP_ASYNC was set.
 */
APDB_rc apdb_env_sync(APDB_Env env, int force);

/**
 * Grow the map ceiling.  No active readers or writers may be open.
 * On Windows a reopen may be required.
 */
APDB_rc apdb_env_set_map_size(APDB_Env env, uint64_t new_max_bytes);

/** Current map ceiling in bytes. */
uint64_t apdb_env_map_size(APDB_Env env);

/** Maximum allowed key size in bytes (a function of page_size). */
size_t apdb_env_max_key_size(APDB_Env env);

/** Reclaim dead-PID reader slots; returns number of slots reclaimed. */
size_t apdb_env_reader_check(APDB_Env env);

/* --------------------------------------------------------------------------
 * Statistics / info
 * -------------------------------------------------------------------------- */

/** Per-B+tree statistics (mirrors LMDB MDB_stat). */
typedef struct APDB_Stat {
    uint32_t page_size;
    uint64_t depth;
    uint64_t branch_pages;
    uint64_t leaf_pages;
    uint64_t overflow_pages;
    uint64_t entries;
} APDB_Stat;

/** Environment info (mirrors LMDB MDB_envinfo). */
typedef struct APDB_EnvInfo {
    uint64_t map_size;
    uint64_t last_page;
    uint64_t last_txn_id;
    uint32_t max_readers;
    uint32_t num_readers;
} APDB_EnvInfo;

/** Statistics for the master (default) keyspace. */
APDB_rc apdb_env_stat(APDB_Env env, APDB_Stat *stat_out);

/** Environment info. */
void apdb_env_info(APDB_Env env, APDB_EnvInfo *info_out);

/* --------------------------------------------------------------------------
 * Default-keyspace convenience helpers
 * (each opens its own transaction internally — for multi-op work,
 *  use beginRead / beginWrite explicitly)
 * -------------------------------------------------------------------------- */

/**
 * Get a value from the default keyspace.
 *
 * Buffer lifetime: `*data_out` is valid until the next apdb_get on the same
 * env OR until apdb_env_close.  Copy if you need it longer.
 */
APDB_rc apdb_get(APDB_Env env,
                 const void *key, size_t key_len,
                 const void **data_out, size_t *size_out);

/** Put (insert or overwrite) a key/value pair. */
APDB_rc apdb_put(APDB_Env env,
                 const void *key, size_t key_len,
                 const void *val, size_t val_len);

/** Put without overwriting — returns APDB_KEY_EXISTS if present. */
APDB_rc apdb_put_nooverwrite(APDB_Env env,
                              const void *key, size_t key_len,
                              const void *val, size_t val_len);

/** Delete a key from the default keyspace. */
APDB_rc apdb_del(APDB_Env env, const void *key, size_t key_len);

/* --------------------------------------------------------------------------
 * Blob Storage (Large Objects)
 * -------------------------------------------------------------------------- */

/** Put a large object into the blob store. */
APDB_rc apdb_env_put_blob(APDB_Env env,
                          const void *key, size_t key_len,
                          const void *blob, size_t blob_len,
                          int sync_mode);

/**
 * Get a large object from the blob store.
 * Buffer lifetime: `*data_out` is valid until the next apdb_env_get_blob call
 * on the same env OR until apdb_env_close.
 */
APDB_rc apdb_env_get_blob(APDB_Env env,
                          const void *key, size_t key_len,
                          const void **data_out, size_t *size_out);

/** Delete a large object from the blob store. */
APDB_rc apdb_env_del_blob(APDB_Env env, const void *key, size_t key_len, int sync_mode);

/* --------------------------------------------------------------------------
 * Read transactions
 * -------------------------------------------------------------------------- */

/** Begin a read transaction (snapshot isolation). */
APDB_rc apdb_txn_begin_read(APDB_Env env, APDB_RTxn *txn_out);

/**
 * Get from the default keyspace within a read transaction.
 * Buffer lifetime: valid until the next call on `txn`, or apdb_rtxn_close.
 */
APDB_rc apdb_rtxn_get(APDB_RTxn txn,
                      const void *key, size_t key_len,
                      const void **data_out, size_t *size_out);

/** Non-zero if key exists in the default keyspace. */
APDB_rc apdb_rtxn_contains(APDB_RTxn txn,
                            const void *key, size_t key_len,
                            int *exists_out);

/** Transaction ID (snapshot txn ID). */
uint64_t apdb_rtxn_id(APDB_RTxn txn);

/**
 * Release the reader-table slot without freeing the object (LMDB reset).
 * The transaction must be renewed before use again.
 */
APDB_rc apdb_rtxn_reset(APDB_RTxn txn);

/** Re-acquire a reader-table slot after apdb_rtxn_reset. */
APDB_rc apdb_rtxn_renew(APDB_RTxn txn);

/** Close and free a read transaction. */
APDB_rc apdb_rtxn_close(APDB_RTxn txn);

/* --------------------------------------------------------------------------
 * Write transactions
 * -------------------------------------------------------------------------- */

/** Sync-mode flags for commit / standalone put/del operations. */
#define APDB_SYNC_DURABLE  0   /**< Fully durable (fsync)                  */
#define APDB_SYNC_ASYNC    1   /**< OS-buffered (MDB_MAPASYNC equivalent)   */
#define APDB_SYNC_NOSYNC   2   /**< No sync at all                          */

/** Begin a write transaction (acquires the single-writer lock). */
APDB_rc apdb_txn_begin_write(APDB_Env env, APDB_WTxn *txn_out);

/** Put within a write transaction (default keyspace). */
APDB_rc apdb_wtxn_put(APDB_WTxn txn,
                      const void *key, size_t key_len,
                      const void *val, size_t val_len,
                      int overwrite);

/** Delete within a write transaction (default keyspace). */
APDB_rc apdb_wtxn_del(APDB_WTxn txn,
                      const void *key, size_t key_len);

/**
 * Get (read-your-own-writes) within a write transaction.
 * Buffer lifetime: valid until the next call on `txn`, or commit/abort.
 */
APDB_rc apdb_wtxn_get(APDB_WTxn txn,
                      const void *key, size_t key_len,
                      const void **data_out, size_t *size_out);

/** Non-zero if key exists (read-your-own-writes). */
APDB_rc apdb_wtxn_contains(APDB_WTxn txn,
                            const void *key, size_t key_len,
                            int *exists_out);

/** Transaction ID. */
uint64_t apdb_wtxn_id(APDB_WTxn txn);

/**
 * Commit and release the writer lock.  `txn` is invalid after this call.
 * @param sync_mode  APDB_SYNC_DURABLE / _ASYNC / _NOSYNC.
 */
APDB_rc apdb_wtxn_commit(APDB_WTxn txn, int sync_mode);

/**
 * Two-Phase Commit: Durably prepare without publishing or releasing lock.
 * Must be followed by apdb_wtxn_commit or apdb_wtxn_abort.
 */
APDB_rc apdb_wtxn_prepare(APDB_WTxn txn, int sync_mode);

/**
 * Clean up any lingering prepared transactions (e.g. after a crash).
 * Normally an external coordinator manages this.
 */
APDB_rc apdb_env_rollback_prepared(APDB_Env env);

/**
 * Abort without publishing.  `txn` is invalid after this call.
 * Also invoked automatically by apdb_wtxn_close if never committed.
 */
APDB_rc apdb_wtxn_abort(APDB_WTxn txn);

/** Alias: abort + free.  Safe to call even after commit (no-op). */
APDB_rc apdb_wtxn_close(APDB_WTxn txn);

/* --------------------------------------------------------------------------
 * Cursors (read)
 * -------------------------------------------------------------------------- */

/**
 * Open a read cursor on the default keyspace.
 * The cursor must be closed before its owning transaction is closed.
 */
APDB_rc apdb_rtxn_cursor_open(APDB_RTxn txn, APDB_Cursor *cur_out);

/* --------------------------------------------------------------------------
 * Cursors (write)
 * -------------------------------------------------------------------------- */

/**
 * Open a write-capable cursor on the default keyspace.
 * The cursor must be closed before the transaction is committed/aborted.
 */
APDB_rc apdb_wtxn_cursor_open(APDB_WTxn txn, APDB_Cursor *cur_out);

/* --------------------------------------------------------------------------
 * Cursor navigation (shared read/write cursors)
 * -------------------------------------------------------------------------- */

/** Position on the first entry.  Returns APDB_NOT_FOUND if empty. */
APDB_rc apdb_cursor_first(APDB_Cursor cur);

/** Position on the last entry.  Returns APDB_NOT_FOUND if empty. */
APDB_rc apdb_cursor_last(APDB_Cursor cur);

/** Advance to the next entry.  Returns APDB_NOT_FOUND at end. */
APDB_rc apdb_cursor_next(APDB_Cursor cur);

/** Retreat to the previous entry.  Returns APDB_NOT_FOUND at start. */
APDB_rc apdb_cursor_prev(APDB_Cursor cur);

/**
 * Seek to the first entry with key >= `key` (MDB_SET_RANGE).
 * Returns APDB_NOT_FOUND if no such entry exists.
 */
APDB_rc apdb_cursor_seek(APDB_Cursor cur, const void *key, size_t key_len);

/**
 * Seek to the entry with exactly `key` (MDB_SET).
 * Returns APDB_NOT_FOUND if absent.
 */
APDB_rc apdb_cursor_seek_exact(APDB_Cursor cur,
                               const void *key, size_t key_len);

/** Non-zero if the cursor is on a valid entry. */
int apdb_cursor_valid(APDB_Cursor cur);

/**
 * Return the current key.
 * Buffer lifetime: valid until the next navigation call on this cursor.
 */
APDB_rc apdb_cursor_key(APDB_Cursor cur,
                        const void **key_out, size_t *key_len_out);

/**
 * Return the current value.
 * Buffer lifetime: valid until the next navigation call on this cursor.
 */
APDB_rc apdb_cursor_value(APDB_Cursor cur,
                          const void **val_out, size_t *val_len_out);

/* --------------------------------------------------------------------------
 * Cursor mutations (write cursors only)
 * -------------------------------------------------------------------------- */

/** Cursor put flags. */
#define APDB_CURSOR_NO_OVERWRITE (1u << 0)
#define APDB_CURSOR_APPEND       (1u << 1)  /**< Keys inserted in order     */
#define APDB_CURSOR_CURRENT      (1u << 2)  /**< Replace current entry      */

/** Put (cursor remains positioned on the written entry). */
APDB_rc apdb_cursor_put(APDB_Cursor cur,
                        const void *key, size_t key_len,
                        const void *val, size_t val_len,
                        uint32_t flags);

/** Delete the current entry (cursor advances to next or becomes invalid). */
APDB_rc apdb_cursor_del(APDB_Cursor cur);

/** Close and free a cursor. */
APDB_rc apdb_cursor_close(APDB_Cursor cur);

/* --------------------------------------------------------------------------
 * Named sub-databases
 * -------------------------------------------------------------------------- */

/** Sub-database creation/open flags (mirrors btree::DbFlags). */
#define APDB_DB_NONE         0u
#define APDB_DB_REVERSE_KEY  (1u << 0)  /**< Keys in reverse order          */
#define APDB_DB_DUP_SORT     (1u << 1)  /**< Allow duplicate keys           */
#define APDB_DB_INTEGER_KEY  (1u << 2)  /**< Keys are native integers        */
#define APDB_DB_DUP_FIXED    (1u << 3)  /**< Dup values have fixed size      */
#define APDB_DB_INTEGER_DUP  (1u << 4)  /**< Dup values are native integers  */

/**
 * Create a named sub-database (fails with APDB_KEY_EXISTS if already present).
 * `flags` governs key/value ordering and are persisted in the catalog.
 */
APDB_rc apdb_subdb_create(APDB_Env env, const char *name,
                          uint32_t flags, APDB_SubDb *subdb_out);

/**
 * Open an existing named sub-database.
 * Returns APDB_NOT_FOUND if it has not been created yet.
 */
APDB_rc apdb_subdb_open(APDB_Env env, const char *name,
                        APDB_SubDb *subdb_out);

/**
 * Get-or-create: opens if present, creates if absent.
 * `flags` are only applied at first creation; ignored thereafter
 * (existing catalog flags always win — matches LMDB semantics).
 */
APDB_rc apdb_subdb_open_or_create(APDB_Env env, const char *name,
                                  uint32_t flags, APDB_SubDb *subdb_out);

/**
 * List all named sub-database names.
 * `names_out` is a heap-allocated array of `count_out` NUL-terminated strings.
 * Caller must free each string and then the array:
 *   for (size_t i = 0; i < count; ++i) free(names[i]);
 *   free(names);
 */
APDB_rc apdb_subdb_list(APDB_Env env,
                        char ***names_out, size_t *count_out);

/** Drop (permanently delete) a named sub-database and all its data. */
APDB_rc apdb_subdb_drop(APDB_Env env, const char *name, int sync_mode);

/** Invalidate the in-process handle (does not delete from disk). */
void apdb_subdb_close(APDB_SubDb subdb);

/** Per-sub-database statistics. */
APDB_rc apdb_subdb_stat(APDB_SubDb subdb, APDB_Stat *stat_out);

/* Sub-database single-op helpers (internal transaction per call). */
APDB_rc apdb_subdb_get(APDB_SubDb subdb,
                       const void *key, size_t key_len,
                       const void **data_out, size_t *size_out);

APDB_rc apdb_subdb_put(APDB_SubDb subdb,
                       const void *key, size_t key_len,
                       const void *val, size_t val_len,
                       int overwrite, int sync_mode);

APDB_rc apdb_subdb_del(APDB_SubDb subdb,
                       const void *key, size_t key_len,
                       int sync_mode);

/**
 * Begin a multi-op write transaction bound to this sub-database.
 * Commit/abort via apdb_wtxn_commit / apdb_wtxn_abort as usual.
 */
APDB_rc apdb_subdb_begin_write(APDB_SubDb subdb, APDB_WTxn *txn_out);

/** Begin a read transaction bound to this sub-database. */
APDB_rc apdb_subdb_begin_read(APDB_SubDb subdb, APDB_RTxn *txn_out);

/**
 * Open a write cursor on a sub-database within an existing write transaction.
 * The write transaction must have been opened via apdb_subdb_begin_write or
 * apdb_txn_begin_write on the same environment.
 */
APDB_rc apdb_subdb_cursor_open_write(APDB_SubDb subdb, APDB_WTxn txn,
                                     APDB_Cursor *cur_out);

/** Open a read cursor on a sub-database within an existing read transaction. */
APDB_rc apdb_subdb_cursor_open_read(APDB_SubDb subdb, APDB_RTxn txn,
                                    APDB_Cursor *cur_out);

/* --------------------------------------------------------------------------
 * Changefeed
 * -------------------------------------------------------------------------- */

typedef struct APDB_ChangeEvent {
    uint64_t sequence;
    uint8_t type; /* 1 = Insert, 2 = Update, 3 = Delete */
    uint64_t txn_id;
    const void *key;
    size_t key_len;
    const void *old_value;
    size_t old_value_len;
    const void *new_value;
    size_t new_value_len;
} APDB_ChangeEvent;

/** Live subscription callback (called synchronously during or after commit). */
typedef void (*APDB_ChangeCallback)(const APDB_ChangeEvent *event, void *user_ctx);

/** Subscribe to live changes. Only one active subscriber per environment. */
APDB_rc apdb_changefeed_subscribe(APDB_Env env, APDB_ChangeCallback cb, void *user_ctx);

/** Unsubscribe from live changes. */
APDB_rc apdb_changefeed_unsubscribe(APDB_Env env);

/**
 * Open a durable polling iterator to read changes after `after_token`.
 * Uses a read transaction `txn` for snapshot isolation.
 */
APDB_rc apdb_changefeed_open_poll(APDB_Env env, APDB_RTxn txn,
                                  uint64_t after_token,
                                  APDB_ChangeFeedIterator *iter_out);

/**
 * Advance the iterator and fetch the next change.
 * Returns APDB_NOT_FOUND when exhausted.
 * Buffer lifetime: Pointers in `event_out` are valid until next call.
 */
APDB_rc apdb_changefeed_next(APDB_ChangeFeedIterator iter, APDB_ChangeEvent *event_out);

/** Close the polling iterator. */
APDB_rc apdb_changefeed_close_poll(APDB_ChangeFeedIterator iter);

/* --------------------------------------------------------------------------
 * Owned byte buffer  (for callers who need data to outlive a transaction)
 * -------------------------------------------------------------------------- */
typedef struct APDB_Bytes {
    void   *data;
    size_t  size;
} APDB_Bytes;

/**
 * Copy `size` bytes from `src` into a newly heap-allocated APDB_Bytes.
 * Caller must free with apdb_bytes_free.
 */
APDB_rc apdb_bytes_copy(const void *src, size_t size, APDB_Bytes *out);

/** Free an APDB_Bytes previously filled by apdb_bytes_copy. */
void apdb_bytes_free(APDB_Bytes *b);

/* --------------------------------------------------------------------------
 * Key derivation helper (only available when built with encryption support)
 * -------------------------------------------------------------------------- */
#ifdef ANNPOCKETDB_WITH_ENCRYPTION
/**
 * Derive a 32-byte encryption key from `passphrase` + `salt` using Argon2id.
 * Write result into `key_out[32]`.
 */
APDB_rc apdb_derive_key(const char *passphrase, size_t passphrase_len,
                         const uint8_t *salt, size_t salt_len,
                         uint8_t key_out[32]);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ANNPOCKETDB_C_H */
