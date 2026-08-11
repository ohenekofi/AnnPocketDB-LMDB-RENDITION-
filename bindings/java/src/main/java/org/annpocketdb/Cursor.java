package org.annpocketdb;

public class Cursor implements AutoCloseable {
    private long handle;

    Cursor(long handle) {
        this.handle = handle;
    }

    public boolean first() {
        checkClosed();
        return Native.cursorFirst(handle);
    }

    public boolean next() {
        checkClosed();
        return Native.cursorNext(handle);
    }

    public boolean isValid() {
        checkClosed();
        return Native.cursorValid(handle);
    }

    public byte[] key() {
        checkClosed();
        return Native.cursorKey(handle);
    }

    public byte[] value() {
        checkClosed();
        return Native.cursorValue(handle);
    }

    @Override
    public void close() {
        if (handle != 0) {
            Native.cursorClose(handle);
            handle = 0;
        }
    }

    private void checkClosed() {
        if (handle == 0) {
            throw new IllegalStateException("Cursor is closed");
        }
    }
}
