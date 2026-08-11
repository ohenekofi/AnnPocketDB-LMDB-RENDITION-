/*
 * src/addon.cpp
 *
 * N-API addon for AnnPocketDb.
 *
 * Calls the stable C API (libannpocketdb_c) exclusively — never touches
 * C++ headers directly.  All handles are wrapped in Napi::External with
 * a Finalizer that closes them when the JS GC collects the object.
 *
 * Buffer lifetime: every value returned from the C API (get / cursor key/value)
 * is immediately copied into a Napi::Buffer<uint8_t> before the C-layer
 * scratch pointer can be invalidated.  This matches LMDB's own Node.js
 * binding convention.
 *
 * Error handling: APDB_rc errors are converted to JS Error objects thrown
 * synchronously (the API is synchronous — async wrappers can be added on top).
 */

#include <napi.h>
#include <cstring>
#include <string>
#include "annpocketdb_c/annpocketdb.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void ThrowApdbError(Napi::Env env, APDB_rc rc) {
    std::string msg = apdb_errmsg(rc);
    msg += " (code=" + std::to_string(rc) + ")";
    Napi::Error::New(env, msg).ThrowAsJavaScriptException();
}

#define CHECK_RC(rc) do { \
    if ((rc) != APDB_OK) { ThrowApdbError(env, (rc)); return env.Null(); } \
} while (0)

static Napi::Value WrapBytes(Napi::Env env, const void* data, size_t size) {
    auto buf = Napi::Buffer<uint8_t>::Copy(env,
        reinterpret_cast<const uint8_t*>(data), size);
    return buf;
}

// ---------------------------------------------------------------------------
// Env
// ---------------------------------------------------------------------------

static Napi::Value EnvOpen(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "path (string) required").ThrowAsJavaScriptException();
        return env.Null();
    }
    std::string path = info[0].As<Napi::String>();
    APDB_EnvOptions opts{};  // zero-initialised = all defaults
    if (info.Length() >= 2 && info[1].IsObject()) {
        Napi::Object o = info[1].As<Napi::Object>();
        if (o.Has("flags"))       opts.flags         = o.Get("flags").As<Napi::Number>().Uint32Value();
        if (o.Has("mapSize"))     opts.map_size       = (uint64_t)o.Get("mapSize").As<Napi::Number>().Int64Value();
        if (o.Has("maxReaders"))  opts.max_readers    = o.Get("maxReaders").As<Napi::Number>().Uint32Value();
        if (o.Has("maxNamedDBs")) opts.max_named_dbs  = o.Get("maxNamedDBs").As<Napi::Number>().Uint32Value();
        if (o.Has("pageSize"))    opts.page_size      = o.Get("pageSize").As<Napi::Number>().Uint32Value();
    }
    APDB_Env handle = nullptr;
    CHECK_RC(apdb_env_open(path.c_str(), &opts, &handle));
    return Napi::External<struct APDB_Env_s>::New(env, handle,
        [](Napi::Env, struct APDB_Env_s* h) { apdb_env_close(h); });
}

static Napi::Value EnvClose(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto handle = info[0].As<Napi::External<struct APDB_Env_s>>().Data();
    CHECK_RC(apdb_env_close(handle));
    return env.Undefined();
}

static Napi::Value EnvSync(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto handle = info[0].As<Napi::External<struct APDB_Env_s>>().Data();
    int force = info.Length() >= 2 ? info[1].As<Napi::Boolean>().Value() : 0;
    CHECK_RC(apdb_env_sync(handle, force));
    return env.Undefined();
}

static Napi::Value EnvGet(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto handle = info[0].As<Napi::External<struct APDB_Env_s>>().Data();
    auto keyBuf = info[1].As<Napi::Buffer<uint8_t>>();
    const void* data = nullptr; size_t sz = 0;
    APDB_rc rc = apdb_get(handle, keyBuf.Data(), keyBuf.ByteLength(), &data, &sz);
    if (rc == APDB_NOT_FOUND) return env.Null();
    CHECK_RC(rc);
    return WrapBytes(env, data, sz);
}

static Napi::Value EnvPut(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto handle   = info[0].As<Napi::External<struct APDB_Env_s>>().Data();
    auto keyBuf   = info[1].As<Napi::Buffer<uint8_t>>();
    auto valBuf   = info[2].As<Napi::Buffer<uint8_t>>();
    bool overwrite = info.Length() >= 4 ? info[3].As<Napi::Boolean>().Value() : true;
    if (overwrite) {
        CHECK_RC(apdb_put(handle, keyBuf.Data(), keyBuf.ByteLength(),
                          valBuf.Data(), valBuf.ByteLength()));
    } else {
        CHECK_RC(apdb_put_nooverwrite(handle, keyBuf.Data(), keyBuf.ByteLength(),
                                      valBuf.Data(), valBuf.ByteLength()));
    }
    return env.Undefined();
}

static Napi::Value EnvDel(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto handle  = info[0].As<Napi::External<struct APDB_Env_s>>().Data();
    auto keyBuf  = info[1].As<Napi::Buffer<uint8_t>>();
    CHECK_RC(apdb_del(handle, keyBuf.Data(), keyBuf.ByteLength()));
    return env.Undefined();
}

static Napi::Value EnvStat(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto handle = info[0].As<Napi::External<struct APDB_Env_s>>().Data();
    APDB_Stat s{};
    CHECK_RC(apdb_env_stat(handle, &s));
    auto obj = Napi::Object::New(env);
    obj.Set("pageSize",      Napi::Number::New(env, s.page_size));
    obj.Set("depth",         Napi::Number::New(env, (double)s.depth));
    obj.Set("branchPages",   Napi::Number::New(env, (double)s.branch_pages));
    obj.Set("leafPages",     Napi::Number::New(env, (double)s.leaf_pages));
    obj.Set("overflowPages", Napi::Number::New(env, (double)s.overflow_pages));
    obj.Set("entries",       Napi::Number::New(env, (double)s.entries));
    return obj;
}

static Napi::Value EnvRollbackPrepared(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto handle = info[0].As<Napi::External<struct APDB_Env_s>>().Data();
    APDB_rc rc = apdb_env_rollback_prepared(handle);
    if (rc != APDB_OK && rc != APDB_NOT_FOUND) {
        ThrowApdbError(env, rc);
    }
    return env.Undefined();
}

static Napi::Value EnvPutBlob(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto handle   = info[0].As<Napi::External<struct APDB_Env_s>>().Data();
    auto keyBuf   = info[1].As<Napi::Buffer<uint8_t>>();
    auto blobBuf  = info[2].As<Napi::Buffer<uint8_t>>();
    int syncMode  = info.Length() >= 4 ? info[3].As<Napi::Number>().Int32Value() : APDB_SYNC_DURABLE;
    CHECK_RC(apdb_env_put_blob(handle, keyBuf.Data(), keyBuf.ByteLength(),
                               blobBuf.Data(), blobBuf.ByteLength(), syncMode));
    return env.Undefined();
}

static Napi::Value EnvGetBlob(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto handle = info[0].As<Napi::External<struct APDB_Env_s>>().Data();
    auto keyBuf = info[1].As<Napi::Buffer<uint8_t>>();
    const void* data = nullptr; size_t sz = 0;
    APDB_rc rc = apdb_env_get_blob(handle, keyBuf.Data(), keyBuf.ByteLength(), &data, &sz);
    if (rc == APDB_NOT_FOUND) return env.Null();
    CHECK_RC(rc);
    return WrapBytes(env, data, sz);
}

static Napi::Value EnvDelBlob(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto handle  = info[0].As<Napi::External<struct APDB_Env_s>>().Data();
    auto keyBuf  = info[1].As<Napi::Buffer<uint8_t>>();
    int syncMode = info.Length() >= 3 ? info[2].As<Napi::Number>().Int32Value() : APDB_SYNC_DURABLE;
    CHECK_RC(apdb_env_del_blob(handle, keyBuf.Data(), keyBuf.ByteLength(), syncMode));
    return env.Undefined();
}

// ---------------------------------------------------------------------------
// Read transaction
// ---------------------------------------------------------------------------

static Napi::Value RTxnBegin(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto envH = info[0].As<Napi::External<struct APDB_Env_s>>().Data();
    APDB_RTxn txn = nullptr;
    CHECK_RC(apdb_txn_begin_read(envH, &txn));
    return Napi::External<struct APDB_RTxn_s>::New(env, txn,
        [](Napi::Env, struct APDB_RTxn_s* h) { apdb_rtxn_close(h); });
}

static Napi::Value RTxnGet(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto txn    = info[0].As<Napi::External<struct APDB_RTxn_s>>().Data();
    auto keyBuf = info[1].As<Napi::Buffer<uint8_t>>();
    const void* data = nullptr; size_t sz = 0;
    APDB_rc rc = apdb_rtxn_get(txn, keyBuf.Data(), keyBuf.ByteLength(), &data, &sz);
    if (rc == APDB_NOT_FOUND) return env.Null();
    CHECK_RC(rc);
    return WrapBytes(env, data, sz);
}

static Napi::Value RTxnClose(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto txn = info[0].As<Napi::External<struct APDB_RTxn_s>>().Data();
    CHECK_RC(apdb_rtxn_close(txn));
    return env.Undefined();
}

// ---------------------------------------------------------------------------
// Write transaction
// ---------------------------------------------------------------------------

static Napi::Value WTxnBegin(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto envH = info[0].As<Napi::External<struct APDB_Env_s>>().Data();
    APDB_WTxn txn = nullptr;
    CHECK_RC(apdb_txn_begin_write(envH, &txn));
    return Napi::External<struct APDB_WTxn_s>::New(env, txn,
        [](Napi::Env, struct APDB_WTxn_s* h) { apdb_wtxn_close(h); });
}

static Napi::Value WTxnPut(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto txn    = info[0].As<Napi::External<struct APDB_WTxn_s>>().Data();
    auto keyBuf = info[1].As<Napi::Buffer<uint8_t>>();
    auto valBuf = info[2].As<Napi::Buffer<uint8_t>>();
    bool ow = info.Length() >= 4 ? info[3].As<Napi::Boolean>().Value() : true;
    CHECK_RC(apdb_wtxn_put(txn, keyBuf.Data(), keyBuf.ByteLength(),
                            valBuf.Data(), valBuf.ByteLength(), ow ? 1 : 0));
    return env.Undefined();
}

static Napi::Value WTxnDel(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto txn    = info[0].As<Napi::External<struct APDB_WTxn_s>>().Data();
    auto keyBuf = info[1].As<Napi::Buffer<uint8_t>>();
    CHECK_RC(apdb_wtxn_del(txn, keyBuf.Data(), keyBuf.ByteLength()));
    return env.Undefined();
}

static Napi::Value WTxnGet(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto txn    = info[0].As<Napi::External<struct APDB_WTxn_s>>().Data();
    auto keyBuf = info[1].As<Napi::Buffer<uint8_t>>();
    const void* data = nullptr; size_t sz = 0;
    APDB_rc rc = apdb_wtxn_get(txn, keyBuf.Data(), keyBuf.ByteLength(), &data, &sz);
    if (rc == APDB_NOT_FOUND) return env.Null();
    CHECK_RC(rc);
    return WrapBytes(env, data, sz);
}

static Napi::Value WTxnCommit(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto txn = info[0].As<Napi::External<struct APDB_WTxn_s>>().Data();
    int syncMode = info.Length() >= 2 ? info[1].As<Napi::Number>().Int32Value() : APDB_SYNC_DURABLE;
    CHECK_RC(apdb_wtxn_commit(txn, syncMode));
    return env.Undefined();
}

static Napi::Value WTxnAbort(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto txn = info[0].As<Napi::External<struct APDB_WTxn_s>>().Data();
    CHECK_RC(apdb_wtxn_abort(txn));
    return env.Undefined();
}

static Napi::Value WTxnPrepare(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto txn = info[0].As<Napi::External<struct APDB_WTxn_s>>().Data();
    int syncMode = info.Length() >= 2 ? info[1].As<Napi::Number>().Int32Value() : APDB_SYNC_DURABLE;
    CHECK_RC(apdb_wtxn_prepare(txn, syncMode));
    return env.Undefined();
}

// ---------------------------------------------------------------------------
// Cursor
// ---------------------------------------------------------------------------

static Napi::Value RTxnCursorOpen(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto txn = info[0].As<Napi::External<struct APDB_RTxn_s>>().Data();
    APDB_Cursor cur = nullptr;
    CHECK_RC(apdb_rtxn_cursor_open(txn, &cur));
    return Napi::External<struct APDB_Cursor_s>::New(env, cur,
        [](Napi::Env, struct APDB_Cursor_s* h) { apdb_cursor_close(h); });
}

static Napi::Value WTxnCursorOpen(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto txn = info[0].As<Napi::External<struct APDB_WTxn_s>>().Data();
    APDB_Cursor cur = nullptr;
    CHECK_RC(apdb_wtxn_cursor_open(txn, &cur));
    return Napi::External<struct APDB_Cursor_s>::New(env, cur,
        [](Napi::Env, struct APDB_Cursor_s* h) { apdb_cursor_close(h); });
}

static Napi::Value CursorFirst(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto cur = info[0].As<Napi::External<struct APDB_Cursor_s>>().Data();
    APDB_rc rc = apdb_cursor_first(cur);
    if (rc == APDB_NOT_FOUND) return Napi::Boolean::New(env, false);
    CHECK_RC(rc);
    return Napi::Boolean::New(env, true);
}

static Napi::Value CursorNext(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto cur = info[0].As<Napi::External<struct APDB_Cursor_s>>().Data();
    APDB_rc rc = apdb_cursor_next(cur);
    if (rc == APDB_NOT_FOUND) return Napi::Boolean::New(env, false);
    CHECK_RC(rc);
    return Napi::Boolean::New(env, true);
}

static Napi::Value CursorSeek(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto cur    = info[0].As<Napi::External<struct APDB_Cursor_s>>().Data();
    auto keyBuf = info[1].As<Napi::Buffer<uint8_t>>();
    APDB_rc rc = apdb_cursor_seek(cur, keyBuf.Data(), keyBuf.ByteLength());
    if (rc == APDB_NOT_FOUND) return Napi::Boolean::New(env, false);
    CHECK_RC(rc);
    return Napi::Boolean::New(env, true);
}

static Napi::Value CursorSeekExact(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto cur    = info[0].As<Napi::External<struct APDB_Cursor_s>>().Data();
    auto keyBuf = info[1].As<Napi::Buffer<uint8_t>>();
    APDB_rc rc = apdb_cursor_seek_exact(cur, keyBuf.Data(), keyBuf.ByteLength());
    if (rc == APDB_NOT_FOUND) return Napi::Boolean::New(env, false);
    CHECK_RC(rc);
    return Napi::Boolean::New(env, true);
}

static Napi::Value CursorValid(const Napi::CallbackInfo& info) {
    auto cur = info[0].As<Napi::External<struct APDB_Cursor_s>>().Data();
    return Napi::Boolean::New(info.Env(), apdb_cursor_valid(cur) != 0);
}

static Napi::Value CursorKey(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto cur = info[0].As<Napi::External<struct APDB_Cursor_s>>().Data();
    const void* k = nullptr; size_t kl = 0;
    CHECK_RC(apdb_cursor_key(cur, &k, &kl));
    return WrapBytes(env, k, kl);
}

static Napi::Value CursorValue(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto cur = info[0].As<Napi::External<struct APDB_Cursor_s>>().Data();
    const void* v = nullptr; size_t vl = 0;
    CHECK_RC(apdb_cursor_value(cur, &v, &vl));
    return WrapBytes(env, v, vl);
}

static Napi::Value CursorPut(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto cur    = info[0].As<Napi::External<struct APDB_Cursor_s>>().Data();
    auto keyBuf = info[1].As<Napi::Buffer<uint8_t>>();
    auto valBuf = info[2].As<Napi::Buffer<uint8_t>>();
    uint32_t flags = info.Length() >= 4 ? info[3].As<Napi::Number>().Uint32Value() : 0;
    CHECK_RC(apdb_cursor_put(cur, keyBuf.Data(), keyBuf.ByteLength(),
                              valBuf.Data(), valBuf.ByteLength(), flags));
    return env.Undefined();
}

static Napi::Value CursorDel(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto cur = info[0].As<Napi::External<struct APDB_Cursor_s>>().Data();
    CHECK_RC(apdb_cursor_del(cur));
    return env.Undefined();
}

static Napi::Value CursorClose(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto cur = info[0].As<Napi::External<struct APDB_Cursor_s>>().Data();
    CHECK_RC(apdb_cursor_close(cur));
    return env.Undefined();
}

// ---------------------------------------------------------------------------
// Changefeed
// ---------------------------------------------------------------------------

static Napi::Value ChangeFeedOpenPoll(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto envH = info[0].As<Napi::External<struct APDB_Env_s>>().Data();
    auto txnH = info[1].As<Napi::External<struct APDB_RTxn_s>>().Data();
    uint64_t afterToken = info.Length() >= 3 ? info[2].As<Napi::Number>().Int64Value() : 0;
    APDB_ChangeFeedIterator iter = nullptr;
    CHECK_RC(apdb_changefeed_open_poll(envH, txnH, afterToken, &iter));
    return Napi::External<struct APDB_ChangeFeedIterator_s>::New(env, iter,
        [](Napi::Env, struct APDB_ChangeFeedIterator_s* h) { apdb_changefeed_close_poll(h); });
}

static Napi::Value ChangeFeedNext(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto iter = info[0].As<Napi::External<struct APDB_ChangeFeedIterator_s>>().Data();
    APDB_ChangeEvent ce{};
    APDB_rc rc = apdb_changefeed_next(iter, &ce);
    if (rc == APDB_NOT_FOUND) return env.Null();
    CHECK_RC(rc);
    
    auto obj = Napi::Object::New(env);
    obj.Set("sequence", Napi::Number::New(env, (double)ce.sequence));
    obj.Set("type",     Napi::Number::New(env, ce.type));
    obj.Set("txnId",    Napi::Number::New(env, (double)ce.txn_id));
    obj.Set("key",      WrapBytes(env, ce.key, ce.key_len));
    obj.Set("oldValue", WrapBytes(env, ce.old_value, ce.old_value_len));
    obj.Set("newValue", WrapBytes(env, ce.new_value, ce.new_value_len));
    return obj;
}

static Napi::Value ChangeFeedClosePoll(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto iter = info[0].As<Napi::External<struct APDB_ChangeFeedIterator_s>>().Data();
    CHECK_RC(apdb_changefeed_close_poll(iter));
    return env.Undefined();
}

// ---------------------------------------------------------------------------
// Module initialization
// ---------------------------------------------------------------------------

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    // Constants
    exports.Set("APDB_OK",           Napi::Number::New(env, APDB_OK));
    exports.Set("APDB_NOT_FOUND",    Napi::Number::New(env, APDB_NOT_FOUND));
    exports.Set("APDB_KEY_EXISTS",   Napi::Number::New(env, APDB_KEY_EXISTS));
    exports.Set("SYNC_DURABLE",      Napi::Number::New(env, APDB_SYNC_DURABLE));
    exports.Set("SYNC_ASYNC",        Napi::Number::New(env, APDB_SYNC_ASYNC));
    exports.Set("CURSOR_APPEND",     Napi::Number::New(env, APDB_CURSOR_APPEND));
    exports.Set("CURSOR_NO_OVERWRITE",Napi::Number::New(env, APDB_CURSOR_NO_OVERWRITE));

    // Environment
    exports.Set("envOpen",   Napi::Function::New(env, EnvOpen));
    exports.Set("envClose",  Napi::Function::New(env, EnvClose));
    exports.Set("envSync",   Napi::Function::New(env, EnvSync));
    exports.Set("envGet",    Napi::Function::New(env, EnvGet));
    exports.Set("envPut",    Napi::Function::New(env, EnvPut));
    exports.Set("envDel",    Napi::Function::New(env, EnvDel));
    exports.Set("envStat",   Napi::Function::New(env, EnvStat));
    exports.Set("envRollbackPrepared", Napi::Function::New(env, EnvRollbackPrepared));
    exports.Set("envPutBlob", Napi::Function::New(env, EnvPutBlob));
    exports.Set("envGetBlob", Napi::Function::New(env, EnvGetBlob));
    exports.Set("envDelBlob", Napi::Function::New(env, EnvDelBlob));

    // Read transaction
    exports.Set("rtxnBegin", Napi::Function::New(env, RTxnBegin));
    exports.Set("rtxnGet",   Napi::Function::New(env, RTxnGet));
    exports.Set("rtxnClose", Napi::Function::New(env, RTxnClose));

    // Write transaction
    exports.Set("wtxnBegin",  Napi::Function::New(env, WTxnBegin));
    exports.Set("wtxnPut",    Napi::Function::New(env, WTxnPut));
    exports.Set("wtxnDel",    Napi::Function::New(env, WTxnDel));
    exports.Set("wtxnGet",    Napi::Function::New(env, WTxnGet));
    exports.Set("wtxnCommit", Napi::Function::New(env, WTxnCommit));
    exports.Set("wtxnPrepare", Napi::Function::New(env, WTxnPrepare));
    exports.Set("wtxnAbort",  Napi::Function::New(env, WTxnAbort));

    // Cursor
    exports.Set("rtxnCursorOpen",  Napi::Function::New(env, RTxnCursorOpen));
    exports.Set("wtxnCursorOpen",  Napi::Function::New(env, WTxnCursorOpen));
    exports.Set("cursorFirst",     Napi::Function::New(env, CursorFirst));
    exports.Set("cursorNext",      Napi::Function::New(env, CursorNext));
    exports.Set("cursorSeek",      Napi::Function::New(env, CursorSeek));
    exports.Set("cursorSeekExact", Napi::Function::New(env, CursorSeekExact));
    exports.Set("cursorValid",     Napi::Function::New(env, CursorValid));
    exports.Set("cursorKey",       Napi::Function::New(env, CursorKey));
    exports.Set("cursorValue",     Napi::Function::New(env, CursorValue));
    exports.Set("cursorPut",       Napi::Function::New(env, CursorPut));
    exports.Set("cursorDel",       Napi::Function::New(env, CursorDel));
    exports.Set("cursorClose",     Napi::Function::New(env, CursorClose));

    // Changefeed
    exports.Set("changefeedOpenPoll",  Napi::Function::New(env, ChangeFeedOpenPoll));
    exports.Set("changefeedNext",      Napi::Function::New(env, ChangeFeedNext));
    exports.Set("changefeedClosePoll", Napi::Function::New(env, ChangeFeedClosePoll));

    return exports;
}

NODE_API_MODULE(annpocketdb_native, Init)
