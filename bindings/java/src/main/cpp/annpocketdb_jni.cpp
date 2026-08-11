/*
 * src/main/cpp/annpocketdb_jni.cpp
 *
 * JNI wrapper over the stable C API (libannpocketdb_c).
 * Zero C++ engine headers are included here — this builds purely against
 * jni.h and annpocketdb.h.
 *
 * Buffer lifetime:
 *   apdb_get and apdb_cursor_key/value return pointers valid until the
 *   next call on the handle. We eagerly copy them into a new jbyteArray
 *   before returning to Java.
 *
 * Error handling:
 *   APDB_rc errors are converted to Java exceptions (AnnPocketDbException).
 */

#include <jni.h>
#include <string>
#include <cstring>
#include "annpocketdb_c/annpocketdb.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void ThrowException(JNIEnv* env, APDB_rc rc) {
    if (rc == APDB_OK) return;
    jclass exClass = env->FindClass("org/annpocketdb/AnnPocketDbException");
    if (!exClass) return; // JVM already threw NoClassDefFoundError
    
    std::string msg = apdb_errmsg(rc);
    jmethodID constructor = env->GetMethodID(exClass, "<init>", "(ILjava/lang/String;)V");
    jstring jmsg = env->NewStringUTF(msg.c_str());
    jthrowable ex = (jthrowable)env->NewObject(exClass, constructor, (jint)rc, jmsg);
    env->Throw(ex);
}

#define CHECK_RC(env, rc) do { \
    if ((rc) != APDB_OK) { ThrowException(env, (rc)); return 0; } \
} while (0)

#define CHECK_RC_VOID(env, rc) do { \
    if ((rc) != APDB_OK) { ThrowException(env, (rc)); return; } \
} while (0)

static jbyteArray WrapBytes(JNIEnv* env, const void* data, size_t size) {
    if (!data) return nullptr;
    jbyteArray arr = env->NewByteArray((jsize)size);
    if (!arr) return nullptr; // OutOfMemoryError thrown by JVM
    env->SetByteArrayRegion(arr, 0, (jsize)size, reinterpret_cast<const jbyte*>(data));
    return arr;
}

// Scoped access to a jbyteArray
struct ScopedBytes {
    JNIEnv* env;
    jbyteArray arr;
    jbyte* ptr;
    jsize len;

    ScopedBytes(JNIEnv* env, jbyteArray arr) : env(env), arr(arr) {
        if (arr) {
            ptr = env->GetByteArrayElements(arr, nullptr);
            len = env->GetArrayLength(arr);
        } else {
            ptr = nullptr;
            len = 0;
        }
    }
    ~ScopedBytes() {
        if (arr && ptr) env->ReleaseByteArrayElements(arr, ptr, JNI_ABORT);
    }
    const void* data() const { return ptr; }
    size_t size() const { return len; }
};

// ---------------------------------------------------------------------------
// Env
// ---------------------------------------------------------------------------

extern "C" JNIEXPORT jlong JNICALL
Java_org_annpocketdb_Native_envOpen(JNIEnv* env, jclass, jstring jpath) {
    const char* path = env->GetStringUTFChars(jpath, nullptr);
    APDB_Env handle = nullptr;
    APDB_rc rc = apdb_env_open(path, nullptr, &handle);
    env->ReleaseStringUTFChars(jpath, path);
    if (rc != APDB_OK) {
        ThrowException(env, rc);
        return 0;
    }
    return reinterpret_cast<jlong>(handle);
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_annpocketdb_Native_envOpenWithFlags(JNIEnv* env, jclass, jstring jpath, jint flags) {
    const char* path = env->GetStringUTFChars(jpath, nullptr);
    APDB_Env handle = nullptr;
    APDB_EnvOptions opts{};
    opts.flags = flags;
    APDB_rc rc = apdb_env_open(path, flags ? &opts : nullptr, &handle);
    env->ReleaseStringUTFChars(jpath, path);
    if (rc != APDB_OK) {
        ThrowException(env, rc);
        return 0;
    }
    return reinterpret_cast<jlong>(handle);
}

extern "C" JNIEXPORT void JNICALL
Java_org_annpocketdb_Native_envClose(JNIEnv* env, jclass, jlong handle) {
    CHECK_RC_VOID(env, apdb_env_close(reinterpret_cast<APDB_Env>(handle)));
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_annpocketdb_Native_envGet(JNIEnv* env, jclass, jlong handle, jbyteArray jkey) {
    ScopedBytes key(env, jkey);
    const void* data = nullptr; size_t sz = 0;
    APDB_rc rc = apdb_get(reinterpret_cast<APDB_Env>(handle), key.data(), key.size(), &data, &sz);
    if (rc == APDB_NOT_FOUND) return nullptr;
    CHECK_RC(env, rc);
    return WrapBytes(env, data, sz);
}

extern "C" JNIEXPORT void JNICALL
Java_org_annpocketdb_Native_envPut(JNIEnv* env, jclass, jlong handle, jbyteArray jkey, jbyteArray jval, jboolean overwrite) {
    ScopedBytes key(env, jkey);
    ScopedBytes val(env, jval);
    if (overwrite) {
        CHECK_RC_VOID(env, apdb_put(reinterpret_cast<APDB_Env>(handle), key.data(), key.size(), val.data(), val.size()));
    } else {
        CHECK_RC_VOID(env, apdb_put_nooverwrite(reinterpret_cast<APDB_Env>(handle), key.data(), key.size(), val.data(), val.size()));
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_annpocketdb_Native_envDel(JNIEnv* env, jclass, jlong handle, jbyteArray jkey) {
    ScopedBytes key(env, jkey);
    CHECK_RC_VOID(env, apdb_del(reinterpret_cast<APDB_Env>(handle), key.data(), key.size()));
}

extern "C" JNIEXPORT void JNICALL
Java_org_annpocketdb_Native_envSync(JNIEnv* env, jclass, jlong handle, jboolean force) {
    CHECK_RC_VOID(env, apdb_env_sync(reinterpret_cast<APDB_Env>(handle), force ? 1 : 0));
}

extern "C" JNIEXPORT void JNICALL
Java_org_annpocketdb_Native_envRollbackPrepared(JNIEnv* env, jclass, jlong handle) {
    APDB_rc rc = apdb_env_rollback_prepared(reinterpret_cast<APDB_Env>(handle));
    if (rc != APDB_OK && rc != APDB_NOT_FOUND) {
        ThrowException(env, rc);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_annpocketdb_Native_envPutBlob(JNIEnv* env, jclass, jlong handle, jbyteArray jkey, jbyteArray jblob, jint syncMode) {
    ScopedBytes key(env, jkey);
    ScopedBytes blob(env, jblob);
    CHECK_RC_VOID(env, apdb_env_put_blob(reinterpret_cast<APDB_Env>(handle), key.data(), key.size(), blob.data(), blob.size(), syncMode));
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_annpocketdb_Native_envGetBlob(JNIEnv* env, jclass, jlong handle, jbyteArray jkey) {
    ScopedBytes key(env, jkey);
    const void* data = nullptr; size_t sz = 0;
    APDB_rc rc = apdb_env_get_blob(reinterpret_cast<APDB_Env>(handle), key.data(), key.size(), &data, &sz);
    if (rc == APDB_NOT_FOUND) return nullptr;
    CHECK_RC(env, rc);
    return WrapBytes(env, data, sz);
}

extern "C" JNIEXPORT void JNICALL
Java_org_annpocketdb_Native_envDelBlob(JNIEnv* env, jclass, jlong handle, jbyteArray jkey, jint syncMode) {
    ScopedBytes key(env, jkey);
    CHECK_RC_VOID(env, apdb_env_del_blob(reinterpret_cast<APDB_Env>(handle), key.data(), key.size(), syncMode));
}

// ---------------------------------------------------------------------------
// Read Txn
// ---------------------------------------------------------------------------

extern "C" JNIEXPORT jlong JNICALL
Java_org_annpocketdb_Native_rtxnBegin(JNIEnv* env, jclass, jlong envHandle) {
    APDB_RTxn txn = nullptr;
    CHECK_RC(env, apdb_txn_begin_read(reinterpret_cast<APDB_Env>(envHandle), &txn));
    return reinterpret_cast<jlong>(txn);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_annpocketdb_Native_rtxnGet(JNIEnv* env, jclass, jlong handle, jbyteArray jkey) {
    ScopedBytes key(env, jkey);
    const void* data = nullptr; size_t sz = 0;
    APDB_rc rc = apdb_rtxn_get(reinterpret_cast<APDB_RTxn>(handle), key.data(), key.size(), &data, &sz);
    if (rc == APDB_NOT_FOUND) return nullptr;
    CHECK_RC(env, rc);
    return WrapBytes(env, data, sz);
}

extern "C" JNIEXPORT void JNICALL
Java_org_annpocketdb_Native_rtxnClose(JNIEnv* env, jclass, jlong handle) {
    CHECK_RC_VOID(env, apdb_rtxn_close(reinterpret_cast<APDB_RTxn>(handle)));
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_annpocketdb_Native_rtxnCursorOpen(JNIEnv* env, jclass, jlong txnHandle) {
    APDB_Cursor cur = nullptr;
    CHECK_RC(env, apdb_rtxn_cursor_open(reinterpret_cast<APDB_RTxn>(txnHandle), &cur));
    return reinterpret_cast<jlong>(cur);
}

// ---------------------------------------------------------------------------
// Write Txn
// ---------------------------------------------------------------------------

extern "C" JNIEXPORT jlong JNICALL
Java_org_annpocketdb_Native_wtxnBegin(JNIEnv* env, jclass, jlong envHandle) {
    APDB_WTxn txn = nullptr;
    CHECK_RC(env, apdb_txn_begin_write(reinterpret_cast<APDB_Env>(envHandle), &txn));
    return reinterpret_cast<jlong>(txn);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_annpocketdb_Native_wtxnGet(JNIEnv* env, jclass, jlong handle, jbyteArray jkey) {
    ScopedBytes key(env, jkey);
    const void* data = nullptr; size_t sz = 0;
    APDB_rc rc = apdb_wtxn_get(reinterpret_cast<APDB_WTxn>(handle), key.data(), key.size(), &data, &sz);
    if (rc == APDB_NOT_FOUND) return nullptr;
    CHECK_RC(env, rc);
    return WrapBytes(env, data, sz);
}

extern "C" JNIEXPORT void JNICALL
Java_org_annpocketdb_Native_wtxnPut(JNIEnv* env, jclass, jlong handle, jbyteArray jkey, jbyteArray jval, jboolean overwrite) {
    ScopedBytes key(env, jkey);
    ScopedBytes val(env, jval);
    CHECK_RC_VOID(env, apdb_wtxn_put(reinterpret_cast<APDB_WTxn>(handle), key.data(), key.size(), val.data(), val.size(), overwrite ? 1 : 0));
}

extern "C" JNIEXPORT void JNICALL
Java_org_annpocketdb_Native_wtxnDel(JNIEnv* env, jclass, jlong handle, jbyteArray jkey) {
    ScopedBytes key(env, jkey);
    CHECK_RC_VOID(env, apdb_wtxn_del(reinterpret_cast<APDB_WTxn>(handle), key.data(), key.size()));
}

extern "C" JNIEXPORT void JNICALL
Java_org_annpocketdb_Native_wtxnCommit(JNIEnv* env, jclass, jlong handle, jint syncMode) {
    CHECK_RC_VOID(env, apdb_wtxn_commit(reinterpret_cast<APDB_WTxn>(handle), syncMode));
}

extern "C" JNIEXPORT void JNICALL
Java_org_annpocketdb_Native_wtxnPrepare(JNIEnv* env, jclass, jlong handle, jint syncMode) {
    CHECK_RC_VOID(env, apdb_wtxn_prepare(reinterpret_cast<APDB_WTxn>(handle), syncMode));
}

extern "C" JNIEXPORT void JNICALL
Java_org_annpocketdb_Native_wtxnAbort(JNIEnv* env, jclass, jlong handle) {
    CHECK_RC_VOID(env, apdb_wtxn_abort(reinterpret_cast<APDB_WTxn>(handle)));
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_annpocketdb_Native_wtxnCursorOpen(JNIEnv* env, jclass, jlong txnHandle) {
    APDB_Cursor cur = nullptr;
    CHECK_RC(env, apdb_wtxn_cursor_open(reinterpret_cast<APDB_WTxn>(txnHandle), &cur));
    return reinterpret_cast<jlong>(cur);
}

// ---------------------------------------------------------------------------
// Cursor
// ---------------------------------------------------------------------------

extern "C" JNIEXPORT jboolean JNICALL
Java_org_annpocketdb_Native_cursorFirst(JNIEnv* env, jclass, jlong handle) {
    APDB_rc rc = apdb_cursor_first(reinterpret_cast<APDB_Cursor>(handle));
    if (rc == APDB_NOT_FOUND) return JNI_FALSE;
    CHECK_RC(env, rc);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_annpocketdb_Native_cursorNext(JNIEnv* env, jclass, jlong handle) {
    APDB_rc rc = apdb_cursor_next(reinterpret_cast<APDB_Cursor>(handle));
    if (rc == APDB_NOT_FOUND) return JNI_FALSE;
    CHECK_RC(env, rc);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_annpocketdb_Native_cursorValid(JNIEnv*, jclass, jlong handle) {
    return apdb_cursor_valid(reinterpret_cast<APDB_Cursor>(handle)) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_annpocketdb_Native_cursorKey(JNIEnv* env, jclass, jlong handle) {
    const void* k = nullptr; size_t kl = 0;
    CHECK_RC(env, apdb_cursor_key(reinterpret_cast<APDB_Cursor>(handle), &k, &kl));
    return WrapBytes(env, k, kl);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_annpocketdb_Native_cursorValue(JNIEnv* env, jclass, jlong handle) {
    const void* v = nullptr; size_t vl = 0;
    CHECK_RC(env, apdb_cursor_value(reinterpret_cast<APDB_Cursor>(handle), &v, &vl));
    return WrapBytes(env, v, vl);
}

extern "C" JNIEXPORT void JNICALL
Java_org_annpocketdb_Native_cursorClose(JNIEnv* env, jclass, jlong handle) {
    CHECK_RC_VOID(env, apdb_cursor_close(reinterpret_cast<APDB_Cursor>(handle)));
}

// ---------------------------------------------------------------------------
// Changefeed
// ---------------------------------------------------------------------------

extern "C" JNIEXPORT jlong JNICALL
Java_org_annpocketdb_Native_changefeedOpenPoll(JNIEnv* env, jclass, jlong envHandle, jlong txnHandle, jlong afterToken) {
    APDB_ChangeFeedIterator iter = nullptr;
    CHECK_RC(env, apdb_changefeed_open_poll(reinterpret_cast<APDB_Env>(envHandle), reinterpret_cast<APDB_RTxn>(txnHandle), (uint64_t)afterToken, &iter));
    return reinterpret_cast<jlong>(iter);
}

extern "C" JNIEXPORT jobject JNICALL
Java_org_annpocketdb_Native_changefeedNext(JNIEnv* env, jclass, jlong handle) {
    APDB_ChangeEvent ce{};
    APDB_rc rc = apdb_changefeed_next(reinterpret_cast<APDB_ChangeFeedIterator>(handle), &ce);
    if (rc == APDB_NOT_FOUND) return nullptr;
    CHECK_RC(env, rc);
    
    jclass clazz = env->FindClass("org/annpocketdb/ChangeEvent");
    if (!clazz) return nullptr;
    jmethodID constructor = env->GetMethodID(clazz, "<init>", "(JIJ[B[B[B)V");
    
    jbyteArray jkey = WrapBytes(env, ce.key, ce.key_len);
    jbyteArray jold = WrapBytes(env, ce.old_value, ce.old_value_len);
    jbyteArray jnew = WrapBytes(env, ce.new_value, ce.new_value_len);
    
    return env->NewObject(clazz, constructor, (jlong)ce.sequence, (jint)ce.type, (jlong)ce.txn_id, jkey, jold, jnew);
}

extern "C" JNIEXPORT void JNICALL
Java_org_annpocketdb_Native_changefeedClosePoll(JNIEnv* env, jclass, jlong handle) {
    CHECK_RC_VOID(env, apdb_changefeed_close_poll(reinterpret_cast<APDB_ChangeFeedIterator>(handle)));
}
