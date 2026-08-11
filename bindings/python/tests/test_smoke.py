"""
pytest smoke test for the Python AnnPocketDb binding.

Run from bindings/python/:
    pytest tests/

Requires libannpocketdb_c to be on LD_LIBRARY_PATH (Linux / macOS) or
in ANNPOCKETDB_LIB (all platforms).
"""

from __future__ import annotations

import os
import shutil
import tempfile

import pytest
import annpocketdb as apdb
from annpocketdb import AnnPocketDbError


@pytest.fixture()
def db_path(tmp_path):
    """Provide a fresh temporary directory for each test."""
    p = tmp_path / "testdb"
    p.mkdir()
    yield str(p)
    shutil.rmtree(str(p), ignore_errors=True)


# ---------------------------------------------------------------------------
# 1. Basic open / put / get / delete
# ---------------------------------------------------------------------------

def test_basic_put_get_delete(db_path):
    with apdb.Env.open(db_path) as env:
        env.put(b"hello", b"world")
        assert env.get(b"hello") == b"world"
        assert env.get(b"missing") is None

        env.delete(b"hello")
        assert env.get(b"hello") is None


# ---------------------------------------------------------------------------
# 2. Explicit write transaction — commit
# ---------------------------------------------------------------------------

def test_write_txn_commit(db_path):
    with apdb.Env.open(db_path) as env:
        with env.write() as txn:
            txn.put(b"k1", b"v1")
            txn.put(b"k2", b"v2")
            assert txn.get(b"k1") == b"v1"   # read-your-own-writes
            assert txn.contains(b"k2")

        # Committed: values survive outside the transaction.
        assert env.get(b"k1") == b"v1"
        assert env.get(b"k2") == b"v2"


# ---------------------------------------------------------------------------
# 3. Write transaction — abort on exception
# ---------------------------------------------------------------------------

def test_write_txn_abort_on_exception(db_path):
    with apdb.Env.open(db_path) as env:
        try:
            with env.write() as txn:
                txn.put(b"ephemeral", b"data")
                raise RuntimeError("simulated failure")
        except RuntimeError:
            pass

        assert env.get(b"ephemeral") is None


# ---------------------------------------------------------------------------
# 4. Read transaction — snapshot isolation
# ---------------------------------------------------------------------------

def test_read_txn_snapshot(db_path):
    with apdb.Env.open(db_path) as env:
        env.put(b"snap", b"before")

        with env.read() as rtxn:
            # Write after the read txn opened — must not be visible.
            env.put(b"snap", b"after")
            assert rtxn.get(b"snap") == b"before"
            assert rtxn.txn_id > 0


# ---------------------------------------------------------------------------
# 5. Cursor — forward iteration
# ---------------------------------------------------------------------------

def test_cursor_forward_iteration(db_path):
    items = [(f"key{i:03}".encode(), f"val{i}".encode()) for i in range(5)]
    with apdb.Env.open(db_path) as env:
        with env.write() as txn:
            for k, v in items:
                txn.put(k, v)

        with env.read() as rtxn:
            collected = list(rtxn.cursor())

    assert len(collected) == 5
    assert [k for k, _ in collected] == [k for k, _ in items]


# ---------------------------------------------------------------------------
# 6. Cursor seek
# ---------------------------------------------------------------------------

def test_cursor_seek(db_path):
    with apdb.Env.open(db_path) as env:
        for i in range(10):
            env.put(f"k{i:02}".encode(), b"x")

        with env.read() as rtxn:
            with rtxn.cursor() as cur:
                assert cur.seek(b"k05")
                assert cur.key() == b"k05"

                assert cur.seek(b"k99")   # past the end
                assert not cur.valid()


# ---------------------------------------------------------------------------
# 7. Write cursor — append, delete
# ---------------------------------------------------------------------------

def test_write_cursor_put_del(db_path):
    with apdb.Env.open(db_path) as env:
        with env.write() as txn:
            with txn.cursor() as cur:
                cur.put(b"c1", b"A", append=True)
                cur.put(b"c2", b"B", append=True)
                cur.put(b"c3", b"C", append=True)
                cur.seek_exact(b"c2")
                cur.delete()

        assert env.get(b"c1") == b"A"
        assert env.get(b"c2") is None
        assert env.get(b"c3") == b"C"


# ---------------------------------------------------------------------------
# 8. put no-overwrite
# ---------------------------------------------------------------------------

def test_put_no_overwrite(db_path):
    with apdb.Env.open(db_path) as env:
        env.put(b"dup", b"first", overwrite=False)
        with pytest.raises(AnnPocketDbError) as exc:
            env.put(b"dup", b"second", overwrite=False)
        assert exc.value.code == apdb._capi.APDB_KEY_EXISTS


# ---------------------------------------------------------------------------
# 9. Sub-database CRUD
# ---------------------------------------------------------------------------

def test_subdb_crud(db_path):
    with apdb.Env.open(db_path) as env:
        with env.create_subdb("myns") as sdb:
            sdb.put(b"sk1", b"sv1")
            sdb.put(b"sk2", b"sv2")
            assert sdb.get(b"sk1") == b"sv1"
            sdb.delete(b"sk1")
            assert sdb.get(b"sk1") is None

        # Re-open
        with env.open_subdb("myns") as sdb2:
            assert sdb2.get(b"sk2") == b"sv2"

        assert "myns" in env.list_subdbs()
        env.drop_subdb("myns")


# ---------------------------------------------------------------------------
# 10. Stat / info
# ---------------------------------------------------------------------------

def test_stat_info(db_path):
    with apdb.Env.open(db_path) as env:
        env.put(b"s", b"v")
        s = env.stat()
        assert s["page_size"] > 0
        assert s["entries"] >= 1

        inf = env.info()
        assert inf["map_size"] > 0
        assert inf["max_readers"] > 0

        assert env.max_key_size > 0
        assert env.map_size > 0


# ---------------------------------------------------------------------------
# 11. Cursor iterator protocol
# ---------------------------------------------------------------------------

def test_cursor_iter_protocol(db_path):
    pairs = {f"k{i}".encode(): f"v{i}".encode() for i in range(3)}
    with apdb.Env.open(db_path) as env:
        with env.write() as txn:
            for k, v in pairs.items():
                txn.put(k, v)

        with env.read() as rtxn:
            with rtxn.cursor() as cur:
                result = dict(cur)  # uses __iter__

    assert result == pairs


# ---------------------------------------------------------------------------
# 12. 2PC
# ---------------------------------------------------------------------------

def test_2pc(db_path):
    with apdb.Env.open(db_path) as env:
        with env.write() as txn:
            txn.put(b"2pc_k", b"2pc_v")
            txn.prepare()
        assert env.get(b"2pc_k") == b"2pc_v"
        env.rollback_prepared()  # no lingering prepared txns

# ---------------------------------------------------------------------------
# 13. Blob storage
# ---------------------------------------------------------------------------

def test_blob_storage(db_path):
    with apdb.Env.open(db_path) as env:
        payload = b"huge_blob_payload" * 10
        env.put_blob(b"b1", payload)
        assert env.get_blob(b"b1") == payload
        env.delete_blob(b"b1")
        assert env.get_blob(b"b1") is None

# ---------------------------------------------------------------------------
# 14. Changefeed
# ---------------------------------------------------------------------------

def test_changefeed(db_path):
    with apdb.Env.open(db_path, flags=apdb.EnvFlags.CHANGEFEED) as env:
        env.put(b"cf_k1", b"cf_v1")
        env.put(b"cf_k1", b"cf_v2")
        env.delete(b"cf_k1")

        with env.read() as rtxn:
            cf = env.open_changefeed(rtxn, 0)
            events = list(cf)
            cf.close()

        assert len(events) == 3
        assert events[0].key == b"cf_k1"
        assert events[0].type == 1  # insert
        assert events[1].new_value == b"cf_v2"
        assert events[1].type == 2  # update
        assert events[2].type == 3  # delete
