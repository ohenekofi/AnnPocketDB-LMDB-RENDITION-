package org.annpocketdb;

public class WriteTxn implements AutoCloseable {
    private long handle;
    private boolean done = false;

    WriteTxn(long handle) {
        this.handle = handle;
    }

    public void put(byte[] key, byte[] val) {
        checkDone();
        Native.wtxnPut(handle, key, val, true);
    }

    public void putNoOverwrite(byte[] key, byte[] val) {
        checkDone();
        Native.wtxnPut(handle, key, val, false);
    }

    public void delete(byte[] key) {
        checkDone();
        Native.wtxnDel(handle, key);
    }

    public byte[] get(byte[] key) {
        checkDone();
        return Native.wtxnGet(handle, key);
    }

    public Cursor openCursor() {
        checkDone();
        return new Cursor(Native.wtxnCursorOpen(handle));
    }

    public void commit() {
        if (done) throw new IllegalStateException("Transaction already finished");
        Native.wtxnCommit(handle, Native.SYNC_DURABLE);
        done = true;
        handle = 0;
    }

    public void commit(int syncMode) {
        checkClosed();
        Native.wtxnCommit(handle, syncMode);
        done = true;
        handle = 0;
    }

    public void prepare() {
        prepare(Native.SYNC_DURABLE);
    }

    public void prepare(int syncMode) {
        checkClosed();
        Native.wtxnPrepare(handle, syncMode);
    }

    public void abort() {
        if (done) return;
        Native.wtxnAbort(handle);
        done = true;
        handle = 0;
    }

    @Override
    public void close() {
        if (!done) {
            abort();
        }
    }

    private void checkDone() {
        if (done) {
            throw new IllegalStateException("Transaction already finished");
        }
    }
}
