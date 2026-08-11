package org.annpocketdb;

public class ReadTxn implements AutoCloseable {
    private long handle;

    ReadTxn(long handle) {
        this.handle = handle;
    }

    public byte[] get(byte[] key) {
        checkClosed();
        return Native.rtxnGet(handle, key);
    }

    public Cursor openCursor() {
        checkClosed();
        return new Cursor(Native.rtxnCursorOpen(handle));
    }

    @Override
    public void close() {
        if (handle != 0) {
            Native.rtxnClose(handle);
            handle = 0;
        }
    }

    private void checkClosed() {
        if (handle == 0) {
            throw new IllegalStateException("ReadTxn is closed");
        }
    }
}
