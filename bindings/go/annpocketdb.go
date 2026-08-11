// Package annpocketdb provides Go bindings for the AnnPocketDb embedded database.
//
// It uses cgo to call the stable C API (libannpocketdb_c) and wraps it in
// idiomatic Go types with proper resource management via runtime.SetFinalizer.
//
// Build requirements:
//
//	CGO_ENABLED=1
//	APDB_LIB_DIR pointing to the directory containing libannpocketdb_c.so
//	  (or set CGO_LDFLAGS / CGO_CFLAGS explicitly)
//
// Example:
//
//	env, err := annpocketdb.Open("/var/db/myapp", nil)
//	if err != nil {
//	    log.Fatal(err)
//	}
//	defer env.Close()
//
//	if err := env.Put([]byte("hello"), []byte("world")); err != nil {
//	    log.Fatal(err)
//	}
//	val, err := env.Get([]byte("hello"))
package annpocketdb

/*
#cgo CFLAGS:  -I${SRCDIR}/../../include
#cgo LDFLAGS: -lannpocketdb_c

#include "annpocketdb_c/annpocketdb.h"
#include <stdlib.h>
*/
import "C"

import (
	"errors"
	"fmt"
	"runtime"
	"unsafe"
)

// ---------------------------------------------------------------------------
// Error type
// ---------------------------------------------------------------------------

// Error is returned by every AnnPocketDb operation that fails.
type Error struct {
	Code int
	Msg  string
}

func (e *Error) Error() string {
	return fmt.Sprintf("annpocketdb[%d]: %s", e.Code, e.Msg)
}

// ErrNotFound is returned when a key does not exist.
var ErrNotFound = &Error{Code: C.APDB_NOT_FOUND, Msg: "key not found"}

// ErrKeyExists is returned on a no-overwrite collision.
var ErrKeyExists = &Error{Code: C.APDB_KEY_EXISTS, Msg: "key already exists"}

func toErr(rc C.APDB_rc) error {
	if rc == C.APDB_OK {
		return nil
	}
	if rc == C.APDB_NOT_FOUND {
		return ErrNotFound
	}
	if rc == C.APDB_KEY_EXISTS {
		return ErrKeyExists
	}
	msg := C.GoString(C.apdb_errmsg(rc))
	return &Error{Code: int(rc), Msg: msg}
}

// ---------------------------------------------------------------------------
// Environment options
// ---------------------------------------------------------------------------

// EnvOptions configures an environment at open time.
type EnvOptions struct {
	Flags          uint32
	MapSize        uint64
	MaxReaders     uint32
	MaxNamedDBs    uint32
	PageSize       uint32
	EncryptionKey  []byte // must be exactly 32 bytes when non-nil
	ComprMinSize   uint32
}

// Env flag constants (mirror APDB_ENV_* in the C header).
const (
	EnvReadOnly      = uint32(C.APDB_ENV_RDONLY)
	EnvNoSync        = uint32(C.APDB_ENV_NO_SYNC)
	EnvNoMetaSync    = uint32(C.APDB_ENV_NO_META_SYNC)
	EnvMapAsync      = uint32(C.APDB_ENV_MAP_ASYNC)
	EnvNoLock        = uint32(C.APDB_ENV_NO_LOCK)
	EnvWAL           = uint32(C.APDB_ENV_WAL)
	EnvCompression   = uint32(C.APDB_ENV_COMPRESSION)
	EnvEncryption    = uint32(C.APDB_ENV_ENCRYPTION)
	EnvDurableCRC    = uint32(C.APDB_ENV_DURABLE_CRC)
	EnvChangefeed      = uint32(C.APDB_ENV_CHANGEFEED)
	EnvChangefeedAsync = uint32(C.APDB_ENV_CHANGEFEED_ASYNC)
)

// Sync-mode constants.
const (
	SyncDurable = int(C.APDB_SYNC_DURABLE)
	SyncAsync   = int(C.APDB_SYNC_ASYNC)
	SyncNoSync  = int(C.APDB_SYNC_NOSYNC)
)

// DB (sub-database) flag constants.
const (
	DBNone       = uint32(C.APDB_DB_NONE)
	DBReverseKey = uint32(C.APDB_DB_REVERSE_KEY)
	DBDupSort    = uint32(C.APDB_DB_DUP_SORT)
	DBIntegerKey = uint32(C.APDB_DB_INTEGER_KEY)
	DBDupFixed   = uint32(C.APDB_DB_DUP_FIXED)
	DBIntegerDup = uint32(C.APDB_DB_INTEGER_DUP)
)

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------

// Env is a handle to an open AnnPocketDb environment.
// It is safe to use from multiple goroutines.
// Always call Close() when done (or use defer).
type Env struct {
	ptr C.APDB_Env
}

// Open opens or creates an AnnPocketDb environment at path.
// opts may be nil to use all defaults.
func Open(path string, opts *EnvOptions) (*Env, error) {
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))

	var copts *C.APDB_EnvOptions
	var coptsVal C.APDB_EnvOptions
	if opts != nil {
		coptsVal.flags         = C.uint32_t(opts.Flags)
		coptsVal.map_size      = C.uint64_t(opts.MapSize)
		coptsVal.max_readers   = C.uint32_t(opts.MaxReaders)
		coptsVal.max_named_dbs = C.uint32_t(opts.MaxNamedDBs)
		coptsVal.page_size     = C.uint32_t(opts.PageSize)
		coptsVal.compression_min_size = C.uint32_t(opts.ComprMinSize)
		if opts.EncryptionKey != nil {
			if len(opts.EncryptionKey) != 32 {
				return nil, errors.New("EncryptionKey must be exactly 32 bytes")
			}
			coptsVal.encryption_key = (*C.uint8_t)(unsafe.Pointer(&opts.EncryptionKey[0]))
		}
		copts = &coptsVal
	}

	var envPtr C.APDB_Env
	rc := C.apdb_env_open(cpath, copts, &envPtr)
	if err := toErr(rc); err != nil {
		return nil, err
	}
	e := &Env{ptr: envPtr}
	runtime.SetFinalizer(e, (*Env).Close)
	return e, nil
}

// Close closes the environment and frees all resources.
func (e *Env) Close() error {
	if e.ptr == nil {
		return nil
	}
	rc := C.apdb_env_close(e.ptr)
	e.ptr = nil
	runtime.SetFinalizer(e, nil)
	return toErr(rc)
}

// Sync flushes outstanding OS buffers to disk.
// force=true overrides NO_SYNC/MAP_ASYNC for this call.
func (e *Env) Sync(force bool) error {
	f := C.int(0)
	if force {
		f = 1
	}
	return toErr(C.apdb_env_sync(e.ptr, f))
}

// SetMapSize grows the virtual address-space ceiling.
func (e *Env) SetMapSize(newMaxBytes uint64) error {
	return toErr(C.apdb_env_set_map_size(e.ptr, C.uint64_t(newMaxBytes)))
}

// MapSize returns the current virtual address-space ceiling.
func (e *Env) MapSize() uint64 { return uint64(C.apdb_env_map_size(e.ptr)) }

// MaxKeySize returns the maximum allowed key size in bytes.
func (e *Env) MaxKeySize() int { return int(C.apdb_env_max_key_size(e.ptr)) }

// ReaderCheck reclaims dead-PID reader slots and returns the count.
func (e *Env) ReaderCheck() int { return int(C.apdb_env_reader_check(e.ptr)) }

// Stat holds per-tree statistics.
type Stat struct {
	PageSize     uint32
	Depth        uint64
	BranchPages  uint64
	LeafPages    uint64
	OverflowPages uint64
	Entries      uint64
}

// Stat returns statistics for the master keyspace.
func (e *Env) Stat() (Stat, error) {
	var cs C.APDB_Stat
	rc := C.apdb_env_stat(e.ptr, &cs)
	if err := toErr(rc); err != nil {
		return Stat{}, err
	}
	return Stat{
		PageSize:      uint32(cs.page_size),
		Depth:         uint64(cs.depth),
		BranchPages:   uint64(cs.branch_pages),
		LeafPages:     uint64(cs.leaf_pages),
		OverflowPages: uint64(cs.overflow_pages),
		Entries:       uint64(cs.entries),
	}, nil
}

// EnvInfo holds environment-level information.
type EnvInfo struct {
	MapSize    uint64
	LastPage   uint64
	LastTxnID  uint64
	MaxReaders uint32
	NumReaders uint32
}

// Info returns environment-level information.
func (e *Env) Info() EnvInfo {
	var ci C.APDB_EnvInfo
	C.apdb_env_info(e.ptr, &ci)
	return EnvInfo{
		MapSize:    uint64(ci.map_size),
		LastPage:   uint64(ci.last_page),
		LastTxnID:  uint64(ci.last_txn_id),
		MaxReaders: uint32(ci.max_readers),
		NumReaders: uint32(ci.num_readers),
	}
}

// ---- Single-op helpers (open their own transaction internally) ----

// RollbackPrepared cleans up any lingering prepared transactions (e.g. after a crash).
func (e *Env) RollbackPrepared() error {
	return toErr(C.apdb_env_rollback_prepared(e.ptr))
}

// Get retrieves the value for key from the default keyspace.
// Returns (nil, nil) when the key does not exist.
func (e *Env) Get(key []byte) ([]byte, error) {
	var dataPtr unsafe.Pointer
	var dataLen C.size_t
	rc := C.apdb_get(e.ptr,
		unsafe.Pointer(&key[0]), C.size_t(len(key)),
		&dataPtr, &dataLen)
	if rc == C.APDB_NOT_FOUND {
		return nil, nil
	}
	if err := toErr(rc); err != nil {
		return nil, err
	}
	// Copy eagerly: library scratch is invalidated on next call.
	return C.GoBytes(dataPtr, C.int(dataLen)), nil
}

// Put inserts or replaces a key/value pair in the default keyspace.
func (e *Env) Put(key, value []byte) error {
	return toErr(C.apdb_put(e.ptr,
		unsafe.Pointer(&key[0]), C.size_t(len(key)),
		unsafe.Pointer(&value[0]), C.size_t(len(value))))
}

// PutNX inserts key/value only if the key does not already exist.
// Returns ErrKeyExists if the key is already present.
func (e *Env) PutNX(key, value []byte) error {
	return toErr(C.apdb_put_nooverwrite(e.ptr,
		unsafe.Pointer(&key[0]), C.size_t(len(key)),
		unsafe.Pointer(&value[0]), C.size_t(len(value))))
}

// Delete removes a key from the default keyspace.
func (e *Env) Delete(key []byte) error {
	return toErr(C.apdb_del(e.ptr,
		unsafe.Pointer(&key[0]), C.size_t(len(key))))
}

// PutBlob stores a large object chain.
func (e *Env) PutBlob(key, blob []byte, syncMode int) error {
	return toErr(C.apdb_env_put_blob(e.ptr,
		unsafe.Pointer(&key[0]), C.size_t(len(key)),
		unsafe.Pointer(&blob[0]), C.size_t(len(blob)),
		C.int(syncMode)))
}

// GetBlob retrieves a large object chain. Returns (nil, nil) if not found.
func (e *Env) GetBlob(key []byte) ([]byte, error) {
	var dataPtr unsafe.Pointer
	var dataLen C.size_t
	rc := C.apdb_env_get_blob(e.ptr,
		unsafe.Pointer(&key[0]), C.size_t(len(key)),
		&dataPtr, &dataLen)
	if rc == C.APDB_NOT_FOUND {
		return nil, nil
	}
	if err := toErr(rc); err != nil {
		return nil, err
	}
	return C.GoBytes(dataPtr, C.int(dataLen)), nil
}

// DeleteBlob removes a large object chain.
func (e *Env) DeleteBlob(key []byte, syncMode int) error {
	return toErr(C.apdb_env_del_blob(e.ptr,
		unsafe.Pointer(&key[0]), C.size_t(len(key)), C.int(syncMode)))
}

// ---- Explicit transactions ----

// BeginRead begins a snapshot-isolated read transaction.
func (e *Env) BeginRead() (*ReadTxn, error) {
	var ptr C.APDB_RTxn
	if err := toErr(C.apdb_txn_begin_read(e.ptr, &ptr)); err != nil {
		return nil, err
	}
	t := &ReadTxn{ptr: ptr}
	runtime.SetFinalizer(t, (*ReadTxn).Close)
	return t, nil
}

// BeginWrite begins a write transaction (acquires the single-writer lock).
func (e *Env) BeginWrite() (*WriteTxn, error) {
	var ptr C.APDB_WTxn
	if err := toErr(C.apdb_txn_begin_write(e.ptr, &ptr)); err != nil {
		return nil, err
	}
	t := &WriteTxn{ptr: ptr}
	runtime.SetFinalizer(t, (*WriteTxn).Close)
	return t, nil
}

// ---- Sub-databases ----

// CreateSubDB creates a new named sub-database.
func (e *Env) CreateSubDB(name string, flags uint32) (*SubDB, error) {
	cname := C.CString(name)
	defer C.free(unsafe.Pointer(cname))
	var ptr C.APDB_SubDb
	if err := toErr(C.apdb_subdb_create(e.ptr, cname, C.uint32_t(flags), &ptr)); err != nil {
		return nil, err
	}
	s := &SubDB{ptr: ptr}
	runtime.SetFinalizer(s, (*SubDB).Close)
	return s, nil
}

// OpenSubDB opens an existing named sub-database.
func (e *Env) OpenSubDB(name string) (*SubDB, error) {
	cname := C.CString(name)
	defer C.free(unsafe.Pointer(cname))
	var ptr C.APDB_SubDb
	if err := toErr(C.apdb_subdb_open(e.ptr, cname, &ptr)); err != nil {
		return nil, err
	}
	s := &SubDB{ptr: ptr}
	runtime.SetFinalizer(s, (*SubDB).Close)
	return s, nil
}

// OpenOrCreateSubDB opens or creates a named sub-database.
func (e *Env) OpenOrCreateSubDB(name string, flags uint32) (*SubDB, error) {
	cname := C.CString(name)
	defer C.free(unsafe.Pointer(cname))
	var ptr C.APDB_SubDb
	if err := toErr(C.apdb_subdb_open_or_create(e.ptr, cname, C.uint32_t(flags), &ptr)); err != nil {
		return nil, err
	}
	s := &SubDB{ptr: ptr}
	runtime.SetFinalizer(s, (*SubDB).Close)
	return s, nil
}

// ListSubDBs returns the names of all named sub-databases.
func (e *Env) ListSubDBs() ([]string, error) {
	var namesPtr **C.char
	var count C.size_t
	if err := toErr(C.apdb_subdb_list(e.ptr, &namesPtr, &count)); err != nil {
		return nil, err
	}
	n := int(count)
	result := make([]string, n)
	// namesPtr is a C heap array of n C strings.
	slice := (*[1 << 28]*C.char)(unsafe.Pointer(namesPtr))[:n:n]
	for i, cs := range slice {
		result[i] = C.GoString(cs)
		C.free(unsafe.Pointer(cs))
	}
	if namesPtr != nil {
		C.free(unsafe.Pointer(namesPtr))
	}
	return result, nil
}

// DropSubDB permanently deletes a named sub-database.
func (e *Env) DropSubDB(name string, syncMode int) error {
	cname := C.CString(name)
	defer C.free(unsafe.Pointer(cname))
	return toErr(C.apdb_subdb_drop(e.ptr, cname, C.int(syncMode)))
}

// ---------------------------------------------------------------------------
// Read transaction
// ---------------------------------------------------------------------------

// ReadTxn is a snapshot-isolated read transaction.
// Call Close() when done (or use defer).
type ReadTxn struct {
	ptr C.APDB_RTxn
}

// Close releases the reader-table slot and frees the transaction.
func (t *ReadTxn) Close() error {
	if t.ptr == nil {
		return nil
	}
	rc := C.apdb_rtxn_close(t.ptr)
	t.ptr = nil
	runtime.SetFinalizer(t, nil)
	return toErr(rc)
}

// TxnID returns the snapshot transaction ID.
func (t *ReadTxn) TxnID() uint64 { return uint64(C.apdb_rtxn_id(t.ptr)) }

// Get retrieves the value for key within this snapshot.
// Returns (nil, nil) when not found.
func (t *ReadTxn) Get(key []byte) ([]byte, error) {
	var dataPtr unsafe.Pointer
	var dataLen C.size_t
	rc := C.apdb_rtxn_get(t.ptr,
		unsafe.Pointer(&key[0]), C.size_t(len(key)),
		&dataPtr, &dataLen)
	if rc == C.APDB_NOT_FOUND {
		return nil, nil
	}
	if err := toErr(rc); err != nil {
		return nil, err
	}
	return C.GoBytes(dataPtr, C.int(dataLen)), nil
}

// Contains reports whether key exists within this snapshot.
func (t *ReadTxn) Contains(key []byte) (bool, error) {
	var exists C.int
	rc := C.apdb_rtxn_contains(t.ptr,
		unsafe.Pointer(&key[0]), C.size_t(len(key)), &exists)
	if err := toErr(rc); err != nil {
		return false, err
	}
	return exists != 0, nil
}

// OpenCursor opens a read cursor on the default keyspace.
func (t *ReadTxn) OpenCursor() (*Cursor, error) {
	var ptr C.APDB_Cursor
	if err := toErr(C.apdb_rtxn_cursor_open(t.ptr, &ptr)); err != nil {
		return nil, err
	}
	c := &Cursor{ptr: ptr}
	runtime.SetFinalizer(c, (*Cursor).Close)
	return c, nil
}

// Reset releases the reader-table slot (LMDB mdb_txn_reset).
func (t *ReadTxn) Reset() error { return toErr(C.apdb_rtxn_reset(t.ptr)) }

// Renew re-acquires a reader-table slot after Reset (LMDB mdb_txn_renew).
func (t *ReadTxn) Renew() error { return toErr(C.apdb_rtxn_renew(t.ptr)) }

// ---------------------------------------------------------------------------
// Write transaction
// ---------------------------------------------------------------------------

// WriteTxn is a write transaction (holds the single-writer lock).
// Must be used only from the goroutine that created it.
// Call Commit() or Abort() when done; Close() aborts if neither was called.
type WriteTxn struct {
	ptr  C.APDB_WTxn
	done bool
}

// Close aborts the transaction if not already committed/aborted.
func (t *WriteTxn) Close() error {
	if t.ptr == nil {
		return nil
	}
	rc := C.apdb_wtxn_close(t.ptr)
	t.ptr = nil
	t.done = true
	runtime.SetFinalizer(t, nil)
	return toErr(rc)
}

// TxnID returns the write transaction ID.
func (t *WriteTxn) TxnID() uint64 { return uint64(C.apdb_wtxn_id(t.ptr)) }

// Put inserts or replaces a key/value pair (read-your-own-writes).
func (t *WriteTxn) Put(key, value []byte, overwrite bool) error {
	ow := C.int(0)
	if overwrite {
		ow = 1
	}
	return toErr(C.apdb_wtxn_put(t.ptr,
		unsafe.Pointer(&key[0]), C.size_t(len(key)),
		unsafe.Pointer(&value[0]), C.size_t(len(value)), ow))
}

// Delete removes a key (read-your-own-writes).
func (t *WriteTxn) Delete(key []byte) error {
	return toErr(C.apdb_wtxn_del(t.ptr,
		unsafe.Pointer(&key[0]), C.size_t(len(key))))
}

// Get reads a key within this write transaction (read-your-own-writes).
// Returns (nil, nil) when not found.
func (t *WriteTxn) Get(key []byte) ([]byte, error) {
	var dataPtr unsafe.Pointer
	var dataLen C.size_t
	rc := C.apdb_wtxn_get(t.ptr,
		unsafe.Pointer(&key[0]), C.size_t(len(key)),
		&dataPtr, &dataLen)
	if rc == C.APDB_NOT_FOUND {
		return nil, nil
	}
	if err := toErr(rc); err != nil {
		return nil, err
	}
	return C.GoBytes(dataPtr, C.int(dataLen)), nil
}

// OpenCursor opens a write-capable cursor on the default keyspace.
func (t *WriteTxn) OpenCursor() (*Cursor, error) {
	var ptr C.APDB_Cursor
	if err := toErr(C.apdb_wtxn_cursor_open(t.ptr, &ptr)); err != nil {
		return nil, err
	}
	c := &Cursor{ptr: ptr}
	runtime.SetFinalizer(c, (*Cursor).Close)
	return c, nil
}

// Commit commits and releases the writer lock.
func (t *WriteTxn) Commit(syncMode int) error {
	if t.done {
		return fmt.Errorf("transaction already finished")
	}
	rc := C.apdb_wtxn_commit(t.ptr, C.int(syncMode))
	t.ptr = nil
	t.done = true
	runtime.SetFinalizer(t, nil)
	return toErr(rc)
}

// Prepare durably prepares a two-phase commit without finalizing.
// Must be followed by Commit() or Abort().
func (t *WriteTxn) Prepare(syncMode int) error {
	if t.done {
		return fmt.Errorf("transaction already finished")
	}
	return toErr(C.apdb_wtxn_prepare(t.ptr, C.int(syncMode)))
}

// Abort aborts without publishing.
func (t *WriteTxn) Abort() error {
	if t.done {
		return nil
	}
	rc := C.apdb_wtxn_abort(t.ptr)
	t.ptr = nil
	t.done = true
	runtime.SetFinalizer(t, nil)
	return toErr(rc)
}

// ---------------------------------------------------------------------------
// Cursor
// ---------------------------------------------------------------------------

// Cursor iterates over a keyspace in key order.
// Close it before the owning transaction is committed/aborted.
type Cursor struct {
	ptr C.APDB_Cursor
}

// Close frees the cursor.
func (c *Cursor) Close() error {
	if c.ptr == nil {
		return nil
	}
	rc := C.apdb_cursor_close(c.ptr)
	c.ptr = nil
	runtime.SetFinalizer(c, nil)
	return toErr(rc)
}

func (c *Cursor) First() error { return toErr(C.apdb_cursor_first(c.ptr)) }
func (c *Cursor) Last()  error { return toErr(C.apdb_cursor_last(c.ptr)) }
func (c *Cursor) Next()  error { return toErr(C.apdb_cursor_next(c.ptr)) }
func (c *Cursor) Prev()  error { return toErr(C.apdb_cursor_prev(c.ptr)) }

func (c *Cursor) Seek(key []byte) error {
	return toErr(C.apdb_cursor_seek(c.ptr,
		unsafe.Pointer(&key[0]), C.size_t(len(key))))
}

func (c *Cursor) SeekExact(key []byte) error {
	return toErr(C.apdb_cursor_seek_exact(c.ptr,
		unsafe.Pointer(&key[0]), C.size_t(len(key))))
}

// Valid reports whether the cursor is positioned on a valid entry.
func (c *Cursor) Valid() bool { return C.apdb_cursor_valid(c.ptr) != 0 }

// Key returns a copy of the current key.
func (c *Cursor) Key() ([]byte, error) {
	var kPtr unsafe.Pointer
	var kLen C.size_t
	if err := toErr(C.apdb_cursor_key(c.ptr, &kPtr, &kLen)); err != nil {
		return nil, err
	}
	return C.GoBytes(kPtr, C.int(kLen)), nil
}

// Value returns a copy of the current value.
func (c *Cursor) Value() ([]byte, error) {
	var vPtr unsafe.Pointer
	var vLen C.size_t
	if err := toErr(C.apdb_cursor_value(c.ptr, &vPtr, &vLen)); err != nil {
		return nil, err
	}
	return C.GoBytes(vPtr, C.int(vLen)), nil
}

// Put inserts or updates via cursor.
func (c *Cursor) Put(key, value []byte, flags uint32) error {
	return toErr(C.apdb_cursor_put(c.ptr,
		unsafe.Pointer(&key[0]), C.size_t(len(key)),
		unsafe.Pointer(&value[0]), C.size_t(len(value)),
		C.uint32_t(flags)))
}

// Delete removes the current entry.
func (c *Cursor) Delete() error { return toErr(C.apdb_cursor_del(c.ptr)) }

// ---------------------------------------------------------------------------
// Sub-database
// ---------------------------------------------------------------------------

// SubDB is a handle to a named sub-database.
type SubDB struct {
	ptr C.APDB_SubDb
}

// Close invalidates the in-process handle (does not delete data from disk).
func (s *SubDB) Close() {
	if s.ptr != nil {
		C.apdb_subdb_close(s.ptr)
		s.ptr = nil
		runtime.SetFinalizer(s, nil)
	}
}

// Get retrieves a value from this sub-database.
// Returns (nil, nil) when not found.
func (s *SubDB) Get(key []byte) ([]byte, error) {
	var dataPtr unsafe.Pointer
	var dataLen C.size_t
	rc := C.apdb_subdb_get(s.ptr,
		unsafe.Pointer(&key[0]), C.size_t(len(key)),
		&dataPtr, &dataLen)
	if rc == C.APDB_NOT_FOUND {
		return nil, nil
	}
	if err := toErr(rc); err != nil {
		return nil, err
	}
	return C.GoBytes(dataPtr, C.int(dataLen)), nil
}

// Put inserts or replaces a key/value pair.
func (s *SubDB) Put(key, value []byte, overwrite bool, syncMode int) error {
	ow := C.int(0)
	if overwrite {
		ow = 1
	}
	return toErr(C.apdb_subdb_put(s.ptr,
		unsafe.Pointer(&key[0]), C.size_t(len(key)),
		unsafe.Pointer(&value[0]), C.size_t(len(value)),
		ow, C.int(syncMode)))
}

// Delete removes a key from this sub-database.
func (s *SubDB) Delete(key []byte, syncMode int) error {
	return toErr(C.apdb_subdb_del(s.ptr,
		unsafe.Pointer(&key[0]), C.size_t(len(key)),
		C.int(syncMode)))
}

// BeginWrite begins a write transaction bound to this sub-database.
func (s *SubDB) BeginWrite() (*WriteTxn, error) {
	var ptr C.APDB_WTxn
	if err := toErr(C.apdb_subdb_begin_write(s.ptr, &ptr)); err != nil {
		return nil, err
	}
	t := &WriteTxn{ptr: ptr}
	runtime.SetFinalizer(t, (*WriteTxn).Close)
	return t, nil
}

// BeginRead begins a read transaction bound to this sub-database.
func (s *SubDB) BeginRead() (*ReadTxn, error) {
	var ptr C.APDB_RTxn
	if err := toErr(C.apdb_subdb_begin_read(s.ptr, &ptr)); err != nil {
		return nil, err
	}
	t := &ReadTxn{ptr: ptr}
	runtime.SetFinalizer(t, (*ReadTxn).Close)
	return t, nil
}

// ---------------------------------------------------------------------------
// Changefeed
// ---------------------------------------------------------------------------

// ChangeEvent represents a single committed mutation.
type ChangeEvent struct {
	Sequence uint64
	Type     uint8 // 1=Insert, 2=Update, 3=Delete
	TxnID    uint64
	Key      []byte
	OldValue []byte
	NewValue []byte
}

// ChangeFeedIterator reads from the durable changefeed.
type ChangeFeedIterator struct {
	ptr C.APDB_ChangeFeedIterator
}

// OpenChangeFeed opens a polling iterator for changes after the given token.
func (e *Env) OpenChangeFeed(txn *ReadTxn, afterToken uint64) (*ChangeFeedIterator, error) {
	var ptr C.APDB_ChangeFeedIterator
	rc := C.apdb_changefeed_open_poll(e.ptr, txn.ptr, C.uint64_t(afterToken), &ptr)
	if err := toErr(rc); err != nil {
		return nil, err
	}
	iter := &ChangeFeedIterator{ptr: ptr}
	runtime.SetFinalizer(iter, (*ChangeFeedIterator).Close)
	return iter, nil
}

// Next reads the next change event. Returns (nil, nil) when exhausted.
func (i *ChangeFeedIterator) Next() (*ChangeEvent, error) {
	if i.ptr == nil {
		return nil, fmt.Errorf("iterator closed")
	}
	var ce C.APDB_ChangeEvent
	rc := C.apdb_changefeed_next(i.ptr, &ce)
	if rc == C.APDB_NOT_FOUND {
		return nil, nil
	}
	if err := toErr(rc); err != nil {
		return nil, err
	}
	return &ChangeEvent{
		Sequence: uint64(ce.sequence),
		Type:     uint8(ce._type),
		TxnID:    uint64(ce.txn_id),
		Key:      C.GoBytes(ce.key, C.int(ce.key_len)),
		OldValue: C.GoBytes(ce.old_value, C.int(ce.old_value_len)),
		NewValue: C.GoBytes(ce.new_value, C.int(ce.new_value_len)),
	}, nil
}

// Close frees the iterator.
func (i *ChangeFeedIterator) Close() error {
	if i.ptr == nil {
		return nil
	}
	rc := C.apdb_changefeed_close_poll(i.ptr)
	i.ptr = nil
	runtime.SetFinalizer(i, nil)
	return toErr(rc)
}
