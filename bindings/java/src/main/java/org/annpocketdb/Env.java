package org.annpocketdb;

public class Env implements AutoCloseable {
    private long handle;

    private Env(long handle) {
        this.handle = handle;
    }

    public static final int CHANGEFEED = 1 << 9;
    public static final int CHANGEFEED_ASYNC = 1 << 10;

    public static Env open(String path) {
        return open(path, 0);
    }

    public static Env open(String path, int flags) {
        long h = Native.envOpenWithFlags(path, flags);
        return new Env(h);
    }

    public void put(byte[] key, byte[] val) {
        checkClosed();
        Native.envPut(handle, key, val, true);
    }

    public void putNoOverwrite(byte[] key, byte[] val) {
        checkClosed();
        Native.envPut(handle, key, val, false);
    }

    public byte[] get(byte[] key) {
        checkClosed();
        return Native.envGet(handle, key);
    }

    public void delete(byte[] key) {
        checkClosed();
        Native.envDel(handle, key);
    }

    public void sync(boolean force) {
        checkClosed();
        Native.envSync(handle, force);
    }

    public void rollbackPrepared() {
        checkClosed();
        Native.envRollbackPrepared(handle);
    }

    public void putBlob(byte[] key, byte[] blob, int syncMode) {
        checkClosed();
        Native.envPutBlob(handle, key, blob, syncMode);
    }

    public byte[] getBlob(byte[] key) {
        checkClosed();
        return Native.envGetBlob(handle, key);
    }

    public void deleteBlob(byte[] key, int syncMode) {
        checkClosed();
        Native.envDelBlob(handle, key, syncMode);
    }

    public ChangeFeedIterator openChangeFeed(ReadTxn txn, long afterToken) {
        checkClosed();
        long iterHandle = Native.changefeedOpenPoll(handle, txn.getHandle(), afterToken);
        return new ChangeFeedIterator(iterHandle);
    }

    public ReadTxn beginRead() {
        checkClosed();
        return new ReadTxn(Native.rtxnBegin(handle));
    }

    public WriteTxn beginWrite() {
        checkClosed();
        return new WriteTxn(Native.wtxnBegin(handle));
    }

    @Override
    public void close() {
        if (handle != 0) {
            Native.envClose(handle);
            handle = 0;
        }
    }

    private void checkClosed() {
        if (handle == 0) {
            throw new IllegalStateException("Environment is closed");
        }
    }
}
