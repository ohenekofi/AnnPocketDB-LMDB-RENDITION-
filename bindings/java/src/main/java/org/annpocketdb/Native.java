package org.annpocketdb;

/**
 * JNI bindings for the C API.
 * Methods map almost 1:1 to apdb_* C functions.
 */
class Native {
    static {
        // Assume the library is on java.library.path or loaded manually
        System.loadLibrary("annpocketdb_jni");
    }

    // Constants
    static final int APDB_OK = 0;
    static final int APDB_NOT_FOUND = 1;

    static final int SYNC_DURABLE = 0;
    static final int SYNC_ASYNC = 1;

    // Env
    static native long envOpen(String path);
    static native long envOpenWithFlags(String path, int flags);
    static native void envClose(long env);
    static native byte[] envGet(long env, byte[] key);
    static native void envPut(long env, byte[] key, byte[] val, boolean overwrite);
    static native void envDel(long env, byte[] key);
    static native void envSync(long env, boolean force);
    static native void envRollbackPrepared(long env);
    static native void envPutBlob(long env, byte[] key, byte[] blob, int syncMode);
    static native byte[] envGetBlob(long env, byte[] key);
    static native void envDelBlob(long env, byte[] key, int syncMode);

    // Read Txn
    static native long rtxnBegin(long env);
    static native byte[] rtxnGet(long txn, byte[] key);
    static native void rtxnClose(long txn);
    static native long rtxnCursorOpen(long txn);

    // Write Txn
    static native long wtxnBegin(long env);
    static native byte[] wtxnGet(long txn, byte[] key);
    static native void wtxnPut(long txn, byte[] key, byte[] val, boolean overwrite);
    static native void wtxnDel(long txn, byte[] key);
    static native void wtxnCommit(long txn, int syncMode);
    static native void wtxnPrepare(long txn, int syncMode);
    static native void wtxnAbort(long txn);
    static native long wtxnCursorOpen(long txn);

    // Cursor
    static native boolean cursorFirst(long cur);
    static native boolean cursorNext(long cur);
    static native boolean cursorValid(long cur);
    static native byte[] cursorKey(long cur);
    static native byte[] cursorValue(long cur);
    static native void cursorClose(long cur);

    // Changefeed
    static native long changefeedOpenPoll(long env, long txn, long afterToken);
    static native ChangeEvent changefeedNext(long iter);
    static native void changefeedClosePoll(long iter);
}
