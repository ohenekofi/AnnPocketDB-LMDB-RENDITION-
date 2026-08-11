"""
annpocketdb._capi
~~~~~~~~~~~~~~~~~
Low-level ctypes loader for libannpocketdb_c.

All raw C functions are declared here with explicit argtypes / restype.
Nothing above this layer touches ctypes — the public API in __init__.py
calls only the thin typed wrappers exposed here.

Buffer lifetime contract (mirrors LMDB):
  raw_get / raw_cursor_key / raw_cursor_value return a (data_ptr, size)
  tuple pointing into the library's internal scratch buffer.  The pointer
  is valid until the next call on the same handle.  The Python wrapper
  always copies immediately via ctypes.string_at(ptr, size).
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import platform
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Library discovery
# ---------------------------------------------------------------------------

def _find_library() -> ctypes.CDLL:
    """
    Search order (production-safe):
      1. ANNPOCKETDB_LIB env var (absolute path or name).
      2. The directory of this package file (for bundled/installed wheels).
      3. ctypes.util.find_library ("annpocketdb_c").
      4. Hardcoded names for the current platform.
    Raises RuntimeError on failure.
    """
    # 1. Explicit override
    if "ANNPOCKETDB_LIB" in os.environ:
        return ctypes.CDLL(os.environ["ANNPOCKETDB_LIB"])

    # 2. Same directory (bundled wheel)
    pkg_dir = Path(__file__).parent
    for suffix in (".so", ".so.0", ".dll", ".dylib"):
        candidate = pkg_dir / f"libannpocketdb_c{suffix}"
        if candidate.exists():
            return ctypes.CDLL(str(candidate))

    # 3. System search
    name = ctypes.util.find_library("annpocketdb_c")
    if name:
        return ctypes.CDLL(name)

    # 4. Platform defaults
    if sys.platform == "win32":
        lib_name = "annpocketdb_c.dll"
    elif sys.platform == "darwin":
        lib_name = "libannpocketdb_c.dylib"
    else:
        lib_name = "libannpocketdb_c.so"

    try:
        return ctypes.CDLL(lib_name)
    except OSError:
        pass

    raise RuntimeError(
        "Cannot find libannpocketdb_c.  "
        "Set the ANNPOCKETDB_LIB environment variable to the full path of the "
        "shared library, or install the annpocketdb package with bundled libs."
    )


_lib = _find_library()

# ---------------------------------------------------------------------------
# Opaque handle types
# ---------------------------------------------------------------------------

class _OpaqueEnv(ctypes.Structure):
    pass
class _OpaqueRTxn(ctypes.Structure):
    pass
class _OpaqueWTxn(ctypes.Structure):
    pass
class _OpaqueCursor(ctypes.Structure):
    pass
class _OpaqueSubDb(ctypes.Structure):
    pass

EnvPtr    = ctypes.POINTER(_OpaqueEnv)
RTxnPtr   = ctypes.POINTER(_OpaqueRTxn)
WTxnPtr   = ctypes.POINTER(_OpaqueWTxn)
CursorPtr = ctypes.POINTER(_OpaqueCursor)
SubDbPtr  = ctypes.POINTER(_OpaqueSubDb)

class _OpaqueChangeFeedIterator(ctypes.Structure):
    pass
ChangeFeedIteratorPtr = ctypes.POINTER(_OpaqueChangeFeedIterator)


class EnvOptions(ctypes.Structure):
    """Mirrors APDB_EnvOptions."""
    _fields_ = [
        ("flags",                ctypes.c_uint32),
        ("page_size",            ctypes.c_uint32),
        ("map_size",             ctypes.c_uint64),
        ("initial_file_size",    ctypes.c_uint64),
        ("max_readers",          ctypes.c_uint32),
        ("max_named_dbs",        ctypes.c_uint32),
        ("encryption_key",       ctypes.c_void_p),
        ("compression_min_size", ctypes.c_uint32),
    ]

class Stat(ctypes.Structure):
    """Mirrors APDB_Stat."""
    _fields_ = [
        ("page_size",      ctypes.c_uint32),
        ("depth",          ctypes.c_uint64),
        ("branch_pages",   ctypes.c_uint64),
        ("leaf_pages",     ctypes.c_uint64),
        ("overflow_pages", ctypes.c_uint64),
        ("entries",        ctypes.c_uint64),
    ]

class EnvInfo(ctypes.Structure):
    """Mirrors APDB_EnvInfo."""
    _fields_ = [
        ("map_size",    ctypes.c_uint64),
        ("last_page",   ctypes.c_uint64),
        ("last_txn_id", ctypes.c_uint64),
        ("max_readers", ctypes.c_uint32),
        ("num_readers", ctypes.c_uint32),
    ]

class Bytes(ctypes.Structure):
    """Mirrors APDB_Bytes."""
    _fields_ = [
        ("data", ctypes.c_void_p),
        ("size", ctypes.c_size_t),
    ]

class ChangeEvent(ctypes.Structure):
    """Mirrors APDB_ChangeEvent."""
    _fields_ = [
        ("sequence", ctypes.c_uint64),
        ("type", ctypes.c_uint8),
        ("txn_id", ctypes.c_uint64),
        ("key", ctypes.c_void_p),
        ("key_len", ctypes.c_size_t),
        ("old_value", ctypes.c_void_p),
        ("old_value_len", ctypes.c_size_t),
        ("new_value", ctypes.c_void_p),
        ("new_value_len", ctypes.c_size_t),
    ]


# ---------------------------------------------------------------------------
# Error codes
# ---------------------------------------------------------------------------

APDB_OK               = 0
APDB_NOT_FOUND        = 1
APDB_CORRUPTION       = 2
APDB_IO_ERROR         = 3
APDB_INVALID_ARG      = 4
APDB_KEY_EXISTS       = 5
APDB_TXN_CONFLICT     = 6
APDB_READ_ONLY        = 7
APDB_MAP_FULL         = 8
APDB_INCOMPATIBLE     = 9
APDB_BUSY             = 10
APDB_NOT_SUPPORTED    = 11
APDB_INTERNAL         = 12
APDB_PAGE_NOT_FOUND   = 13
APDB_PANIC            = 14
APDB_VERSION_MISMATCH = 15
APDB_INVALID_FILE     = 16
APDB_DBS_FULL         = 17
APDB_READERS_FULL     = 18
APDB_MAP_RESIZED      = 19
APDB_BAD_DBI          = 20
APDB_BAD_TXN          = 21
APDB_BAD_VAL_SIZE     = 22
APDB_LOCKED           = 23
APDB_OUT_OF_MEMORY    = 24
APDB_DISK_FULL        = 25

# Env flags
APDB_ENV_RDONLY       = 1 << 0
APDB_ENV_NO_SYNC      = 1 << 1
APDB_ENV_NO_META_SYNC = 1 << 2
APDB_ENV_MAP_ASYNC    = 1 << 3
APDB_ENV_NO_LOCK      = 1 << 4
APDB_ENV_WAL          = 1 << 5
APDB_ENV_COMPRESSION  = 1 << 6
APDB_ENV_ENCRYPTION   = 1 << 7
APDB_ENV_DURABLE_CRC  = 1 << 8
APDB_ENV_CHANGEFEED       = 1 << 9
APDB_ENV_CHANGEFEED_ASYNC = 1 << 10

# Sync modes
APDB_SYNC_DURABLE = 0
APDB_SYNC_ASYNC   = 1
APDB_SYNC_NOSYNC  = 2

# DB flags
APDB_DB_NONE         = 0
APDB_DB_REVERSE_KEY  = 1 << 0
APDB_DB_DUP_SORT     = 1 << 1
APDB_DB_INTEGER_KEY  = 1 << 2
APDB_DB_DUP_FIXED    = 1 << 3
APDB_DB_INTEGER_DUP  = 1 << 4

# Cursor put flags
APDB_CURSOR_NO_OVERWRITE = 1 << 0
APDB_CURSOR_APPEND       = 1 << 1
APDB_CURSOR_CURRENT      = 1 << 2


# ---------------------------------------------------------------------------
# Function declarations
# ---------------------------------------------------------------------------

def _fn(name, restype, *argtypes):
    f = getattr(_lib, name)
    f.restype  = restype
    f.argtypes = list(argtypes)
    return f


c_int       = ctypes.c_int
c_uint32    = ctypes.c_uint32
c_uint64    = ctypes.c_uint64
c_size_t    = ctypes.c_size_t
c_char_p    = ctypes.c_char_p
c_void_p    = ctypes.c_void_p
POINTER     = ctypes.POINTER

_apdb_version_check  = _fn("apdb_version_check", c_int, c_int, c_int, c_int)
_apdb_errmsg         = _fn("apdb_errmsg", c_char_p, c_int)

_apdb_env_open       = _fn("apdb_env_open", c_int, c_char_p,
                            POINTER(EnvOptions), POINTER(EnvPtr))
_apdb_env_close      = _fn("apdb_env_close", c_int, EnvPtr)
_apdb_env_sync       = _fn("apdb_env_sync", c_int, EnvPtr, c_int)
_apdb_env_set_map_size = _fn("apdb_env_set_map_size", c_int, EnvPtr, c_uint64)
_apdb_env_map_size   = _fn("apdb_env_map_size", c_uint64, EnvPtr)
_apdb_env_max_key_size = _fn("apdb_env_max_key_size", c_size_t, EnvPtr)
_apdb_env_reader_check = _fn("apdb_env_reader_check", c_size_t, EnvPtr)
_apdb_env_stat       = _fn("apdb_env_stat", c_int, EnvPtr, POINTER(Stat))
_apdb_env_info       = _fn("apdb_env_info", None, EnvPtr, POINTER(EnvInfo))
_apdb_env_rollback_prepared = _fn("apdb_env_rollback_prepared", c_int, EnvPtr)

_apdb_env_put_blob   = _fn("apdb_env_put_blob", c_int, EnvPtr, c_void_p, c_size_t, c_void_p, c_size_t, c_int)
_apdb_env_get_blob   = _fn("apdb_env_get_blob", c_int, EnvPtr, c_void_p, c_size_t, POINTER(c_void_p), POINTER(c_size_t))
_apdb_env_del_blob   = _fn("apdb_env_del_blob", c_int, EnvPtr, c_void_p, c_size_t, c_int)

_apdb_get            = _fn("apdb_get", c_int, EnvPtr,
                            c_void_p, c_size_t, POINTER(c_void_p), POINTER(c_size_t))
_apdb_put            = _fn("apdb_put", c_int, EnvPtr,
                            c_void_p, c_size_t, c_void_p, c_size_t)
_apdb_put_nooverwrite= _fn("apdb_put_nooverwrite", c_int, EnvPtr,
                            c_void_p, c_size_t, c_void_p, c_size_t)
_apdb_del            = _fn("apdb_del", c_int, EnvPtr, c_void_p, c_size_t)

_apdb_txn_begin_read = _fn("apdb_txn_begin_read", c_int, EnvPtr, POINTER(RTxnPtr))
_apdb_rtxn_get       = _fn("apdb_rtxn_get", c_int, RTxnPtr,
                            c_void_p, c_size_t, POINTER(c_void_p), POINTER(c_size_t))
_apdb_rtxn_contains  = _fn("apdb_rtxn_contains", c_int, RTxnPtr,
                            c_void_p, c_size_t, POINTER(c_int))
_apdb_rtxn_id        = _fn("apdb_rtxn_id", c_uint64, RTxnPtr)
_apdb_rtxn_reset     = _fn("apdb_rtxn_reset", c_int, RTxnPtr)
_apdb_rtxn_renew     = _fn("apdb_rtxn_renew", c_int, RTxnPtr)
_apdb_rtxn_close     = _fn("apdb_rtxn_close", c_int, RTxnPtr)

_apdb_txn_begin_write = _fn("apdb_txn_begin_write", c_int, EnvPtr, POINTER(WTxnPtr))
_apdb_wtxn_put       = _fn("apdb_wtxn_put", c_int, WTxnPtr,
                            c_void_p, c_size_t, c_void_p, c_size_t, c_int)
_apdb_wtxn_del       = _fn("apdb_wtxn_del", c_int, WTxnPtr, c_void_p, c_size_t)
_apdb_wtxn_get       = _fn("apdb_wtxn_get", c_int, WTxnPtr,
                            c_void_p, c_size_t, POINTER(c_void_p), POINTER(c_size_t))
_apdb_wtxn_contains  = _fn("apdb_wtxn_contains", c_int, WTxnPtr,
                            c_void_p, c_size_t, POINTER(c_int))
_apdb_wtxn_id        = _fn("apdb_wtxn_id", c_uint64, WTxnPtr)
_apdb_wtxn_commit    = _fn("apdb_wtxn_commit", c_int, WTxnPtr, c_int)
_apdb_wtxn_prepare   = _fn("apdb_wtxn_prepare", c_int, WTxnPtr, c_int)
_apdb_wtxn_abort     = _fn("apdb_wtxn_abort", c_int, WTxnPtr)
_apdb_wtxn_close     = _fn("apdb_wtxn_close", c_int, WTxnPtr)

_apdb_rtxn_cursor_open  = _fn("apdb_rtxn_cursor_open", c_int, RTxnPtr, POINTER(CursorPtr))
_apdb_wtxn_cursor_open  = _fn("apdb_wtxn_cursor_open", c_int, WTxnPtr, POINTER(CursorPtr))
_apdb_cursor_first      = _fn("apdb_cursor_first",  c_int, CursorPtr)
_apdb_cursor_last       = _fn("apdb_cursor_last",   c_int, CursorPtr)
_apdb_cursor_next       = _fn("apdb_cursor_next",   c_int, CursorPtr)
_apdb_cursor_prev       = _fn("apdb_cursor_prev",   c_int, CursorPtr)
_apdb_cursor_seek       = _fn("apdb_cursor_seek",   c_int, CursorPtr, c_void_p, c_size_t)
_apdb_cursor_seek_exact = _fn("apdb_cursor_seek_exact", c_int, CursorPtr, c_void_p, c_size_t)
_apdb_cursor_valid      = _fn("apdb_cursor_valid",  c_int, CursorPtr)
_apdb_cursor_key        = _fn("apdb_cursor_key",    c_int, CursorPtr,
                               POINTER(c_void_p), POINTER(c_size_t))
_apdb_cursor_value      = _fn("apdb_cursor_value",  c_int, CursorPtr,
                               POINTER(c_void_p), POINTER(c_size_t))
_apdb_cursor_put        = _fn("apdb_cursor_put",    c_int, CursorPtr,
                               c_void_p, c_size_t, c_void_p, c_size_t, c_uint32)
_apdb_cursor_del        = _fn("apdb_cursor_del",    c_int, CursorPtr)
_apdb_cursor_close      = _fn("apdb_cursor_close",  c_int, CursorPtr)

_apdb_subdb_create          = _fn("apdb_subdb_create", c_int, EnvPtr, c_char_p, c_uint32, POINTER(SubDbPtr))
_apdb_subdb_open            = _fn("apdb_subdb_open",   c_int, EnvPtr, c_char_p, POINTER(SubDbPtr))
_apdb_subdb_open_or_create  = _fn("apdb_subdb_open_or_create", c_int, EnvPtr, c_char_p, c_uint32, POINTER(SubDbPtr))
_apdb_subdb_list            = _fn("apdb_subdb_list", c_int, EnvPtr,
                                   POINTER(POINTER(c_char_p)), POINTER(c_size_t))
_apdb_subdb_drop            = _fn("apdb_subdb_drop", c_int, EnvPtr, c_char_p, c_int)
_apdb_subdb_close           = _fn("apdb_subdb_close", None, SubDbPtr)
_apdb_subdb_stat            = _fn("apdb_subdb_stat", c_int, SubDbPtr, POINTER(Stat))
_apdb_subdb_get             = _fn("apdb_subdb_get", c_int, SubDbPtr,
                                   c_void_p, c_size_t, POINTER(c_void_p), POINTER(c_size_t))
_apdb_subdb_put             = _fn("apdb_subdb_put", c_int, SubDbPtr,
                                   c_void_p, c_size_t, c_void_p, c_size_t, c_int, c_int)
_apdb_subdb_del             = _fn("apdb_subdb_del", c_int, SubDbPtr, c_void_p, c_size_t, c_int)
_apdb_subdb_begin_write     = _fn("apdb_subdb_begin_write", c_int, SubDbPtr, POINTER(WTxnPtr))
_apdb_subdb_begin_read      = _fn("apdb_subdb_begin_read",  c_int, SubDbPtr, POINTER(RTxnPtr))

_apdb_changefeed_open_poll  = _fn("apdb_changefeed_open_poll", c_int, EnvPtr, RTxnPtr, c_uint64, POINTER(ChangeFeedIteratorPtr))
_apdb_changefeed_next       = _fn("apdb_changefeed_next", c_int, ChangeFeedIteratorPtr, POINTER(ChangeEvent))
_apdb_changefeed_close_poll = _fn("apdb_changefeed_close_poll", c_int, ChangeFeedIteratorPtr)

_apdb_bytes_copy = _fn("apdb_bytes_copy", c_int, c_void_p, c_size_t, POINTER(Bytes))
_apdb_bytes_free = _fn("apdb_bytes_free", None, POINTER(Bytes))


# ---------------------------------------------------------------------------
# Typed helpers used by __init__.py
# ---------------------------------------------------------------------------

def errmsg(rc: int) -> str:
    b = _apdb_errmsg(rc)
    return b.decode() if b else f"error {rc}"


def _check(rc: int, expected_not_found_ok: bool = False) -> int:
    """Raise on any error code other than APDB_OK (and optionally NOT_FOUND)."""
    if rc == APDB_OK:
        return rc
    if expected_not_found_ok and rc == APDB_NOT_FOUND:
        return rc
    raise AnnPocketDbError(rc, errmsg(rc))


class AnnPocketDbError(Exception):
    def __init__(self, code: int, msg: str):
        self.code = code
        super().__init__(f"[{code}] {msg}")
