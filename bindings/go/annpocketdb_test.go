package annpocketdb_test

import (
	"os"
	"path/filepath"
	"testing"

	annpocketdb "github.com/annpocketdb/annpocketdb-go"
)

// tempDB creates a fresh temporary directory and returns its path + a cleanup func.
func tempDB(t *testing.T) (string, func()) {
	t.Helper()
	dir, err := os.MkdirTemp("", "apdb_test_*")
	if err != nil {
		t.Fatal(err)
	}
	return filepath.Join(dir, "db"), func() { os.RemoveAll(dir) }
}

// ---------------------------------------------------------------------------
// 1. Basic open / put / get / delete
// ---------------------------------------------------------------------------

func TestBasicPutGetDelete(t *testing.T) {
	path, cleanup := tempDB(t)
	defer cleanup()

	env, err := annpocketdb.Open(path, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer env.Close()

	if err := env.Put([]byte("hello"), []byte("world")); err != nil {
		t.Fatal(err)
	}
	val, err := env.Get([]byte("hello"))
	if err != nil {
		t.Fatal(err)
	}
	if string(val) != "world" {
		t.Fatalf("got %q want %q", val, "world")
	}

	// Missing key returns nil, nil.
	v2, err := env.Get([]byte("missing"))
	if err != nil {
		t.Fatal(err)
	}
	if v2 != nil {
		t.Fatalf("expected nil for missing key, got %q", v2)
	}

	if err := env.Delete([]byte("hello")); err != nil {
		t.Fatal(err)
	}
	v3, _ := env.Get([]byte("hello"))
	if v3 != nil {
		t.Fatal("expected nil after delete")
	}
}

// ---------------------------------------------------------------------------
// 2. Write transaction commit
// ---------------------------------------------------------------------------

func TestWriteTxnCommit(t *testing.T) {
	path, cleanup := tempDB(t)
	defer cleanup()

	env, _ := annpocketdb.Open(path, nil)
	defer env.Close()

	wtxn, err := env.BeginWrite()
	if err != nil {
		t.Fatal(err)
	}
	_ = wtxn.Put([]byte("k1"), []byte("v1"), true)
	_ = wtxn.Put([]byte("k2"), []byte("v2"), true)

	// Read-your-own-writes.
	got, _ := wtxn.Get([]byte("k1"))
	if string(got) != "v1" {
		t.Fatalf("RYOW: got %q want v1", got)
	}

	if err := wtxn.Commit(annpocketdb.SyncDurable); err != nil {
		t.Fatal(err)
	}

	v, _ := env.Get([]byte("k1"))
	if string(v) != "v1" {
		t.Fatalf("after commit: got %q want v1", v)
	}
}

// ---------------------------------------------------------------------------
// 3. Write transaction abort
// ---------------------------------------------------------------------------

func TestWriteTxnAbort(t *testing.T) {
	path, cleanup := tempDB(t)
	defer cleanup()

	env, _ := annpocketdb.Open(path, nil)
	defer env.Close()

	wtxn, _ := env.BeginWrite()
	_ = wtxn.Put([]byte("ephemeral"), []byte("data"), true)
	_ = wtxn.Abort()

	v, _ := env.Get([]byte("ephemeral"))
	if v != nil {
		t.Fatal("aborted key must not be visible")
	}
}

// ---------------------------------------------------------------------------
// 4. Read cursor forward iteration
// ---------------------------------------------------------------------------

func TestReadCursorForward(t *testing.T) {
	path, cleanup := tempDB(t)
	defer cleanup()

	env, _ := annpocketdb.Open(path, nil)
	defer env.Close()

	expected := []string{"a", "b", "c", "d", "e"}
	for _, k := range expected {
		_ = env.Put([]byte(k), []byte("v:"+k))
	}

	rtxn, err := env.BeginRead()
	if err != nil {
		t.Fatal(err)
	}
	defer rtxn.Close()

	cur, err := rtxn.OpenCursor()
	if err != nil {
		t.Fatal(err)
	}
	defer cur.Close()

	if err := cur.First(); err != nil {
		t.Fatal(err)
	}

	var got []string
	for cur.Valid() {
		k, err := cur.Key()
		if err != nil {
			t.Fatal(err)
		}
		got = append(got, string(k))
		if err := cur.Next(); err != nil && err != annpocketdb.ErrNotFound {
			t.Fatal(err)
		}
		if err == annpocketdb.ErrNotFound {
			break
		}
	}

	if len(got) != len(expected) {
		t.Fatalf("iteration got %d keys, want %d", len(got), len(expected))
	}
	for i := range expected {
		if got[i] != expected[i] {
			t.Fatalf("key[%d]: got %q want %q", i, got[i], expected[i])
		}
	}
}

// ---------------------------------------------------------------------------
// 5. PutNX / ErrKeyExists
// ---------------------------------------------------------------------------

func TestPutNX(t *testing.T) {
	path, cleanup := tempDB(t)
	defer cleanup()

	env, _ := annpocketdb.Open(path, nil)
	defer env.Close()

	_ = env.PutNX([]byte("dup"), []byte("first"))
	err := env.PutNX([]byte("dup"), []byte("second"))
	if err != annpocketdb.ErrKeyExists {
		t.Fatalf("expected ErrKeyExists, got %v", err)
	}
}

// ---------------------------------------------------------------------------
// 6. Stat / Info
// ---------------------------------------------------------------------------

func TestStatInfo(t *testing.T) {
	path, cleanup := tempDB(t)
	defer cleanup()

	env, _ := annpocketdb.Open(path, nil)
	defer env.Close()

	_ = env.Put([]byte("s"), []byte("v"))

	st, err := env.Stat()
	if err != nil {
		t.Fatal(err)
	}
	if st.PageSize == 0 {
		t.Fatal("page_size should be > 0")
	}
	if st.Entries == 0 {
		t.Fatal("entries should be > 0")
	}

	inf := env.Info()
	if inf.MapSize == 0 {
		t.Fatal("map_size should be > 0")
	}

	if env.MaxKeySize() == 0 {
		t.Fatal("max_key_size should be > 0")
	}
}

// ---------------------------------------------------------------------------
// 7. Sub-database CRUD
// ---------------------------------------------------------------------------

func TestSubDBCRUD(t *testing.T) {
	path, cleanup := tempDB(t)
	defer cleanup()

	env, _ := annpocketdb.Open(path, nil)
	defer env.Close()

	sdb, err := env.CreateSubDB("ns1", annpocketdb.DBNone)
	if err != nil {
		t.Fatal(err)
	}

	_ = sdb.Put([]byte("sk"), []byte("sv"), true, annpocketdb.SyncDurable)
	got, _ := sdb.Get([]byte("sk"))
	if string(got) != "sv" {
		t.Fatalf("got %q want sv", got)
	}
	sdb.Close()

	// Re-open and verify
	sdb2, err := env.OpenSubDB("ns1")
	if err != nil {
		t.Fatal(err)
	}
	got2, _ := sdb2.Get([]byte("sk"))
	if string(got2) != "sv" {
		t.Fatalf("after reopen got %q want sv", got2)
	}
	sdb2.Close()

	names, _ := env.ListSubDBs()
	found := false
	for _, n := range names {
		if n == "ns1" {
			found = true
		}
	}
	if !found {
		t.Fatal("ns1 not in ListSubDBs")
	}

	_ = env.DropSubDB("ns1", annpocketdb.SyncDurable)
	_, err = env.OpenSubDB("ns1")
	if err != annpocketdb.ErrNotFound {
		t.Fatalf("expected ErrNotFound after drop, got %v", err)
	}
}

// ---------------------------------------------------------------------------
// 8. 2PC
// ---------------------------------------------------------------------------

func Test2PC(t *testing.T) {
	path, cleanup := tempDB(t)
	defer cleanup()

	env, _ := annpocketdb.Open(path, nil)
	defer env.Close()

	wtxn, _ := env.BeginWrite()
	_ = wtxn.Put([]byte("2pc_key"), []byte("val"), true)
	if err := wtxn.Prepare(annpocketdb.SyncDurable); err != nil {
		t.Fatal(err)
	}
	if err := wtxn.Commit(annpocketdb.SyncDurable); err != nil {
		t.Fatal(err)
	}

	got, _ := env.Get([]byte("2pc_key"))
	if string(got) != "val" {
		t.Fatalf("got %q want val", got)
	}

	err := env.RollbackPrepared()
	if err != annpocketdb.ErrNotFound {
		t.Fatalf("expected ErrNotFound for RollbackPrepared, got %v", err)
	}
}

// ---------------------------------------------------------------------------
// 9. Blob Storage
// ---------------------------------------------------------------------------

func TestBlobStorage(t *testing.T) {
	path, cleanup := tempDB(t)
	defer cleanup()

	env, _ := annpocketdb.Open(path, nil)
	defer env.Close()

	blobData := []byte("huge_blob_payload")
	if err := env.PutBlob([]byte("blob_key"), blobData, annpocketdb.SyncDurable); err != nil {
		t.Fatal(err)
	}

	got, err := env.GetBlob([]byte("blob_key"))
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != string(blobData) {
		t.Fatalf("got %q want %q", got, blobData)
	}

	if err := env.DeleteBlob([]byte("blob_key"), annpocketdb.SyncDurable); err != nil {
		t.Fatal(err)
	}
	
	got2, _ := env.GetBlob([]byte("blob_key"))
	if got2 != nil {
		t.Fatal("expected nil after delete blob")
	}
}

// ---------------------------------------------------------------------------
// 10. Changefeed
// ---------------------------------------------------------------------------

func TestChangefeed(t *testing.T) {
	path, cleanup := tempDB(t)
	defer cleanup()

	opts := &annpocketdb.EnvOptions{
		Flags: annpocketdb.EnvChangefeed,
	}
	env, _ := annpocketdb.Open(path, opts)
	defer env.Close()

	_ = env.Put([]byte("k1"), []byte("v1"))
	_ = env.Delete([]byte("k1"))

	rtxn, _ := env.BeginRead()
	defer rtxn.Close()

	iter, err := env.OpenChangeFeed(rtxn, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer iter.Close()

	var events []*annpocketdb.ChangeEvent
	for {
		ev, err := iter.Next()
		if err != nil {
			t.Fatal(err)
		}
		if ev == nil {
			break
		}
		events = append(events, ev)
	}

	if len(events) != 2 {
		t.Fatalf("expected 2 events, got %d", len(events))
	}
	if string(events[0].Key) != "k1" || events[0].Type != 1 {
		t.Fatal("expected insert for k1")
	}
	if string(events[1].Key) != "k1" || events[1].Type != 3 {
		t.Fatal("expected delete for k1")
	}
}
