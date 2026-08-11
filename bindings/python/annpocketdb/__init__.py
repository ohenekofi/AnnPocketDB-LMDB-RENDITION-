"""
annpocketdb
~~~~~~~~~~~
Pythonic wrapper around the AnnPocketDb C shared library.

Typical usage::

    import annpocketdb as apdb

    with apdb.Env.open("/tmp/mydb") as env:
        env.put(b"hello", b"world")
        print(env.get(b"hello"))   # b"world"

        with env.write() as txn:
            txn.put(b"k1", b"v1")
            txn.put(b"k2", b"v2")
            # auto-committed on __exit__; abort on exception

        with env.read() as txn:
            for key, value in txn.cursor():
                print(key, value)
"""

from __future__ import annotations

import ctypes
from typing import Generator, Iterator, Optional, Tuple

from . import _capi as C

__all__ = [
    "Env",
    "ReadTxn",
    "WriteTxn",
    "Cursor",
    "SubDatabase",
    "AnnPocketDbError",
    "EnvFlags",
    "DbFlags",
    "SyncMode",
]

AnnPocketDbError = C.AnnPocketDbError


# ---------------------------------------------------------------------------
# Constants re-exported with Python-friendly names
# ---------------------------------------------------------------------------

class EnvFlags:
    READ_ONLY       = C.APDB_ENV_RDONLY
    NO_SYNC         = C.APDB_ENV_NO_SYNC
    NO_META_SYNC    = C.APDB_ENV_NO_META_SYNC
    MAP_ASYNC       = C.APDB_ENV_MAP_ASYNC
    NO_LOCK         = C.APDB_ENV_NO_LOCK
    WAL             = C.APDB_ENV_WAL
    COMPRESSION     = C.APDB_ENV_COMPRESSION
    ENCRYPTION      = C.APDB_ENV_ENCRYPTION
    DURABLE_CRC     = C.APDB_ENV_DURABLE_CRC
    CHANGEFEED      = C.APDB_ENV_CHANGEFEED
    CHANGEFEED_ASYNC = C.APDB_ENV_CHANGEFEED_ASYNC


class DbFlags:
    NONE            = C.APDB_DB_NONE
    REVERSE_KEY     = C.APDB_DB_REVERSE_KEY
    DUP_SORT        = C.APDB_DB_DUP_SORT
    INTEGER_KEY     = C.APDB_DB_INTEGER_KEY
    DUP_FIXED       = C.APDB_DB_DUP_FIXED
    INTEGER_DUP     = C.APDB_DB_INTEGER_DUP


class SyncMode:
    DURABLE         = C.APDB_SYNC_DURABLE
    ASYNC           = C.APDB_SYNC_ASYNC
    NOSYNC          = C.APDB_SYNC_NOSYNC


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _encode(s: bytes | str) -> bytes:
    return s.encode() if isinstance(s, str) else s


def _raw_get(get_fn, handle, key: bytes) -> Optional[bytes]:
    """
    Call a raw get function and copy the result into Python bytes.
    Returns None on APDB_NOT_FOUND; raises on any other error.
    """
    data_p = C.c_void_p()
    size_p = C.c_size_t()
    rc = get_fn(handle, key, len(key),
                ctypes.byref(data_p), ctypes.byref(size_p))
    if rc == C.APDB_NOT_FOUND:
        return None
    C._check(rc)
    # Eagerly copy: the library's scratch pointer is invalidated on the next call.
    return ctypes.string_at(data_p, size_p.value)


# ---------------------------------------------------------------------------
# Cursor
# ---------------------------------------------------------------------------

class Cursor:
    """
    Ordered iterator over a keyspace.  Supports forward and reverse
    iteration plus seek operations.  Use as a context manager or call
    close() explicitly when done.
    """

    def __init__(self, ptr: C.CursorPtr):
        self._ptr = ptr
        self._closed = False

    # ---- context manager ----

    def __enter__(self) -> "Cursor":
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        if not self._closed and self._ptr:
            C._apdb_cursor_close(self._ptr)
            self._closed = True

    # ---- positioning ----

    def first(self) -> bool:
        rc = C._apdb_cursor_first(self._ptr)
        if rc == C.APDB_NOT_FOUND:
            return False
        C._check(rc)
        return True

    def last(self) -> bool:
        rc = C._apdb_cursor_last(self._ptr)
        if rc == C.APDB_NOT_FOUND:
            return False
        C._check(rc)
        return True

    def next(self) -> bool:
        rc = C._apdb_cursor_next(self._ptr)
        if rc == C.APDB_NOT_FOUND:
            return False
        C._check(rc)
        return True

    def prev(self) -> bool:
        rc = C._apdb_cursor_prev(self._ptr)
        if rc == C.APDB_NOT_FOUND:
            return False
        C._check(rc)
        return True

    def seek(self, key: bytes | str) -> bool:
        """Position at first entry with key >= `key`."""
        k = _encode(key)
        rc = C._apdb_cursor_seek(self._ptr, k, len(k))
        if rc == C.APDB_NOT_FOUND:
            return False
        C._check(rc)
        return True

    def seek_exact(self, key: bytes | str) -> bool:
        """Position at entry with exactly `key`."""
        k = _encode(key)
        rc = C._apdb_cursor_seek_exact(self._ptr, k, len(k))
        if rc == C.APDB_NOT_FOUND:
            return False
        C._check(rc)
        return True

    # ---- access ----

    def valid(self) -> bool:
        return bool(C._apdb_cursor_valid(self._ptr))

    def key(self) -> Optional[bytes]:
        k = C.c_void_p()
        kl = C.c_size_t()
        rc = C._apdb_cursor_key(self._ptr, ctypes.byref(k), ctypes.byref(kl))
        if not self.valid():
            return None
        C._check(rc)
        return ctypes.string_at(k, kl.value)

    def value(self) -> Optional[bytes]:
        v = C.c_void_p()
        vl = C.c_size_t()
        rc = C._apdb_cursor_value(self._ptr, ctypes.byref(v), ctypes.byref(vl))
        if not self.valid():
            return None
        C._check(rc)
        return ctypes.string_at(v, vl.value)

    def item(self) -> Optional[Tuple[bytes, bytes]]:
        """Return (key, value) or None if not positioned."""
        k = self.key()
        v = self.value()
        if k is None:
            return None
        return k, v

    # ---- mutations (write cursors only) ----

    def put(self, key: bytes | str, value: bytes | str,
            *,
            append: bool = False,
            no_overwrite: bool = False,
            current: bool = False) -> None:
        k = _encode(key)
        v = _encode(value)
        flags = 0
        if no_overwrite: flags |= C.APDB_CURSOR_NO_OVERWRITE
        if append:       flags |= C.APDB_CURSOR_APPEND
        if current:      flags |= C.APDB_CURSOR_CURRENT
        C._check(C._apdb_cursor_put(self._ptr, k, len(k), v, len(v), flags))

    def delete(self) -> None:
        C._check(C._apdb_cursor_del(self._ptr))

    # ---- iterator protocol ----

    def __iter__(self) -> Iterator[Tuple[bytes, bytes]]:
        """Yield (key, value) for all entries from current position to end."""
        if not self.valid():
            if not self.first():
                return
        while self.valid():
            item = self.item()
            if item is not None:
                yield item
            if not self.next():
                break


# ---------------------------------------------------------------------------
# Read transaction
# ---------------------------------------------------------------------------

class ReadTxn:
    """
    Snapshot-isolated read transaction.  Use as a context manager; the
    transaction is automatically closed on __exit__.
    """

    def __init__(self, ptr: C.RTxnPtr):
        self._ptr = ptr
        self._closed = False

    def __enter__(self) -> "ReadTxn":
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        if not self._closed and self._ptr:
            C._apdb_rtxn_close(self._ptr)
            self._closed = True

    @property
    def txn_id(self) -> int:
        return C._apdb_rtxn_id(self._ptr)

    def get(self, key: bytes | str) -> Optional[bytes]:
        return _raw_get(C._apdb_rtxn_get, self._ptr, _encode(key))

    def contains(self, key: bytes | str) -> bool:
        k = _encode(key)
        exists = C.c_int(0)
        C._check(C._apdb_rtxn_contains(self._ptr, k, len(k), ctypes.byref(exists)))
        return bool(exists.value)

    def cursor(self) -> Cursor:
        ptr = C.CursorPtr()
        C._check(C._apdb_rtxn_cursor_open(self._ptr, ctypes.byref(ptr)))
        return Cursor(ptr)

    def reset(self) -> None:
        C._check(C._apdb_rtxn_reset(self._ptr))

    def renew(self) -> None:
        C._check(C._apdb_rtxn_renew(self._ptr))


# ---------------------------------------------------------------------------
# Write transaction
# ---------------------------------------------------------------------------

class WriteTxn:
    """
    Write transaction (single-writer lock held for lifetime).
    Use as a context manager: commits on clean __exit__, aborts on exception.
    """

    def __init__(self, ptr: C.WTxnPtr):
        self._ptr = ptr
        self._done = False

    def __enter__(self) -> "WriteTxn":
        return self

    def __exit__(self, exc_type, *_):
        if not self._done:
            if exc_type is None:
                self.commit()
            else:
                self.abort()

    @property
    def txn_id(self) -> int:
        return C._apdb_wtxn_id(self._ptr)

    def put(self, key: bytes | str, value: bytes | str, *, overwrite: bool = True) -> None:
        k, v = _encode(key), _encode(value)
        C._check(C._apdb_wtxn_put(self._ptr, k, len(k), v, len(v), int(overwrite)))

    def delete(self, key: bytes | str) -> None:
        k = _encode(key)
        C._check(C._apdb_wtxn_del(self._ptr, k, len(k)))

    def get(self, key: bytes | str) -> Optional[bytes]:
        return _raw_get(C._apdb_wtxn_get, self._ptr, _encode(key))

    def contains(self, key: bytes | str) -> bool:
        k = _encode(key)
        exists = C.c_int(0)
        C._check(C._apdb_wtxn_contains(self._ptr, k, len(k), ctypes.byref(exists)))
        return bool(exists.value)

    def cursor(self) -> Cursor:
        ptr = C.CursorPtr()
        C._check(C._apdb_wtxn_cursor_open(self._ptr, ctypes.byref(ptr)))
        return Cursor(ptr)

    def commit(self, sync_mode: int = SyncMode.DURABLE) -> None:
        if self._done:
            raise AnnPocketDbError(C.APDB_BAD_TXN, "transaction already finished")
        C._check(C._apdb_wtxn_commit(self._ptr, sync_mode))
        self._done = True

    def prepare(self, sync_mode: int = SyncMode.DURABLE) -> None:
        if self._done:
            raise AnnPocketDbError(C.APDB_BAD_TXN, "transaction already finished")
        C._check(C._apdb_wtxn_prepare(self._ptr, sync_mode))

    def abort(self) -> None:
        if not self._done:
            C._apdb_wtxn_abort(self._ptr)
            self._done = True


# ---------------------------------------------------------------------------
# Sub-database
# ---------------------------------------------------------------------------

class SubDatabase:
    """
    Handle to a named sub-database inside an environment.
    Single-op helpers open their own internal transaction per call;
    for multi-op use, call begin_write() / begin_read().
    """

    def __init__(self, ptr: C.SubDbPtr):
        self._ptr = ptr

    def close(self):
        if self._ptr:
            C._apdb_subdb_close(self._ptr)
            self._ptr = None

    def __enter__(self) -> "SubDatabase":
        return self

    def __exit__(self, *_):
        self.close()

    def get(self, key: bytes | str) -> Optional[bytes]:
        return _raw_get(C._apdb_subdb_get, self._ptr, _encode(key))

    def put(self, key: bytes | str, value: bytes | str,
            *, overwrite: bool = True, sync: int = SyncMode.DURABLE) -> None:
        k, v = _encode(key), _encode(value)
        C._check(C._apdb_subdb_put(self._ptr, k, len(k), v, len(v),
                                    int(overwrite), sync))

    def delete(self, key: bytes | str, *, sync: int = SyncMode.DURABLE) -> None:
        k = _encode(key)
        C._check(C._apdb_subdb_del(self._ptr, k, len(k), sync))

    def begin_write(self) -> WriteTxn:
        ptr = C.WTxnPtr()
        C._check(C._apdb_subdb_begin_write(self._ptr, ctypes.byref(ptr)))
        return WriteTxn(ptr)

    def begin_read(self) -> ReadTxn:
        ptr = C.RTxnPtr()
        C._check(C._apdb_subdb_begin_read(self._ptr, ctypes.byref(ptr)))
        return ReadTxn(ptr)


# ---------------------------------------------------------------------------
# Environment
# ---------------------------------------------------------------------------

class Env:
    """
    Top-level environment handle.

    Example::

        with Env.open("/var/db/myapp") as env:
            env.put(b"key", b"value")
            with env.write() as txn:
                txn.put(b"a", b"1")
                txn.put(b"b", b"2")
    """

    def __init__(self, ptr: C.EnvPtr):
        self._ptr = ptr
        self._closed = False

    # ---- construction ----

    @classmethod
    def open(cls,
             path: str | bytes,
             *,
             flags: int = 0,
             map_size: int = 0,
             max_readers: int = 0,
             max_named_dbs: int = 0,
             page_size: int = 0,
             encryption_key: Optional[bytes] = None) -> "Env":
        """
        Open or create an AnnPocketDb environment.

        :param path:           Database directory.
        :param flags:          EnvFlags bitmask.
        :param map_size:       Virtual map ceiling in bytes (0 = 1 TiB default).
        :param max_readers:    Max concurrent readers (0 = 256).
        :param max_named_dbs:  Max named sub-databases (0 = unlimited).
        :param page_size:      Page size in bytes (0 = 4096).
        :param encryption_key: 32-byte key (requires APDB_ENV_ENCRYPTION in flags).
        """
        opts = C.EnvOptions()
        opts.flags         = flags
        opts.map_size      = map_size
        opts.max_readers   = max_readers
        opts.max_named_dbs = max_named_dbs
        opts.page_size     = page_size
        if encryption_key is not None:
            if len(encryption_key) != 32:
                raise ValueError("encryption_key must be exactly 32 bytes")
            key_arr = (C.ctypes.c_uint8 * 32)(*encryption_key)
            opts.encryption_key = ctypes.cast(key_arr, C.c_void_p)

        if isinstance(path, str):
            path = path.encode()

        env_ptr = C.EnvPtr()
        C._check(C._apdb_env_open(path, ctypes.byref(opts),
                                   ctypes.byref(env_ptr)))
        return cls(env_ptr)

    # ---- context manager ----

    def __enter__(self) -> "Env":
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        if not self._closed and self._ptr:
            C._apdb_env_close(self._ptr)
            self._closed = True

    # ---- single-op helpers (own internal transaction per call) ----

    def rollback_prepared(self) -> None:
        """Rollback any lingering prepared transactions."""
        C._check(C._apdb_env_rollback_prepared(self._ptr), expected_not_found_ok=True)

    def get(self, key: bytes | str) -> Optional[bytes]:
        """Return value for `key`, or None if not found."""
        return _raw_get(C._apdb_get, self._ptr, _encode(key))

    def put(self, key: bytes | str, value: bytes | str,
            *, overwrite: bool = True) -> None:
        k, v = _encode(key), _encode(value)
        if overwrite:
            C._check(C._apdb_put(self._ptr, k, len(k), v, len(v)))
        else:
            rc = C._apdb_put_nooverwrite(self._ptr, k, len(k), v, len(v))
            C._check(rc)

    def delete(self, key: bytes | str) -> None:
        k = _encode(key)
        C._check(C._apdb_del(self._ptr, k, len(k)))

    def put_blob(self, key: bytes | str, blob: bytes, *, sync_mode: int = SyncMode.DURABLE) -> None:
        k = _encode(key)
        C._check(C._apdb_env_put_blob(self._ptr, k, len(k), blob, len(blob), sync_mode))

    def get_blob(self, key: bytes | str) -> Optional[bytes]:
        return _raw_get(C._apdb_env_get_blob, self._ptr, _encode(key))

    def delete_blob(self, key: bytes | str, *, sync_mode: int = SyncMode.DURABLE) -> None:
        k = _encode(key)
        C._check(C._apdb_env_del_blob(self._ptr, k, len(k), sync_mode))

    # ---- explicit transactions ----

    def read(self) -> ReadTxn:
        """Begin a read transaction (use as context manager)."""
        ptr = C.RTxnPtr()
        C._check(C._apdb_txn_begin_read(self._ptr, ctypes.byref(ptr)))
        return ReadTxn(ptr)

    def write(self) -> WriteTxn:
        """Begin a write transaction (use as context manager)."""
        ptr = C.WTxnPtr()
        C._check(C._apdb_txn_begin_write(self._ptr, ctypes.byref(ptr)))
        return WriteTxn(ptr)

    # ---- sub-databases ----

    def create_subdb(self, name: str, *, flags: int = DbFlags.NONE) -> SubDatabase:
        ptr = C.SubDbPtr()
        C._check(C._apdb_subdb_create(self._ptr, name.encode(),
                                       flags, ctypes.byref(ptr)))
        return SubDatabase(ptr)

    def open_subdb(self, name: str) -> SubDatabase:
        ptr = C.SubDbPtr()
        C._check(C._apdb_subdb_open(self._ptr, name.encode(), ctypes.byref(ptr)))
        return SubDatabase(ptr)

    def open_or_create_subdb(self, name: str, *, flags: int = DbFlags.NONE) -> SubDatabase:
        ptr = C.SubDbPtr()
        C._check(C._apdb_subdb_open_or_create(self._ptr, name.encode(),
                                               flags, ctypes.byref(ptr)))
        return SubDatabase(ptr)

    def list_subdbs(self) -> list[str]:
        names_p = ctypes.POINTER(ctypes.c_char_p)()
        count = C.c_size_t(0)
        C._check(C._apdb_subdb_list(self._ptr,
                                     ctypes.byref(names_p),
                                     ctypes.byref(count)))
        result = []
        for i in range(count.value):
            result.append(names_p[i].decode())
            C.ctypes.cast(names_p[i], C.c_void_p)  # keep alive
        # free each string then the array
        import ctypes as _ct
        for i in range(count.value):
            _ct.cdll.msvcrt.free(names_p[i]) if hasattr(_ct.cdll, 'msvcrt') else None
        return result

    def drop_subdb(self, name: str, *, sync: int = SyncMode.DURABLE) -> None:
        C._check(C._apdb_subdb_drop(self._ptr, name.encode(), sync))

    # ---- env management ----

    def sync(self, *, force: bool = False) -> None:
        C._check(C._apdb_env_sync(self._ptr, int(force)))

    def set_map_size(self, new_max_bytes: int) -> None:
        C._check(C._apdb_env_set_map_size(self._ptr, new_max_bytes))

    @property
    def map_size(self) -> int:
        return C._apdb_env_map_size(self._ptr)

    @property
    def max_key_size(self) -> int:
        return C._apdb_env_max_key_size(self._ptr)

    def reader_check(self) -> int:
        return C._apdb_env_reader_check(self._ptr)

    def stat(self) -> dict:
        s = C.Stat()
        C._check(C._apdb_env_stat(self._ptr, ctypes.byref(s)))
        return {
            "page_size":      s.page_size,
            "depth":          s.depth,
            "branch_pages":   s.branch_pages,
            "leaf_pages":     s.leaf_pages,
            "overflow_pages": s.overflow_pages,
            "entries":        s.entries,
        }

    def info(self) -> dict:
        inf = C.EnvInfo()
        C._apdb_env_info(self._ptr, ctypes.byref(inf))
        return {
            "map_size":    inf.map_size,
            "last_page":   inf.last_page,
            "last_txn_id": inf.last_txn_id,
            "max_readers": inf.max_readers,
            "num_readers": inf.num_readers,
        }

# ---------------------------------------------------------------------------
# Changefeed
# ---------------------------------------------------------------------------

class ChangeEvent:
    def __init__(self, c_event: C.ChangeEvent):
        self.sequence: int = c_event.sequence
        self.type: int = c_event.type
        self.txn_id: int = c_event.txn_id
        self.key: bytes = ctypes.string_at(c_event.key, c_event.key_len) if c_event.key else b""
        self.old_value: bytes = ctypes.string_at(c_event.old_value, c_event.old_value_len) if c_event.old_value else b""
        self.new_value: bytes = ctypes.string_at(c_event.new_value, c_event.new_value_len) if c_event.new_value else b""

class ChangeFeedIterator:
    def __init__(self, ptr: C.ChangeFeedIteratorPtr):
        self._ptr = ptr
        self._closed = False

    def close(self):
        if not self._closed and self._ptr:
            C._apdb_changefeed_close_poll(self._ptr)
            self._closed = True

    def __iter__(self):
        return self

    def __next__(self) -> ChangeEvent:
        if self._closed or not self._ptr:
            raise StopIteration
        ce = C.ChangeEvent()
        rc = C._apdb_changefeed_next(self._ptr, ctypes.byref(ce))
        if rc == C.APDB_NOT_FOUND:
            raise StopIteration
        C._check(rc)
        return ChangeEvent(ce)

def _env_open_changefeed(self: Env, txn: ReadTxn, after_token: int = 0) -> ChangeFeedIterator:
    ptr = C.ChangeFeedIteratorPtr()
    C._check(C._apdb_changefeed_open_poll(self._ptr, txn._ptr, after_token, ctypes.byref(ptr)))
    return ChangeFeedIterator(ptr)

Env.open_changefeed = _env_open_changefeed
