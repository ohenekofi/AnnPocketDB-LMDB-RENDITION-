//
// AnnPocketDb :: api/environment_blob.cpp
//
// Environment::putBlob/getBlob/removeBlob -- split out of environment.cpp
// (kernel tier) into the features tier, alongside blob_store.cpp, so that
// annpocketdb_kernel never needs a symbol from annpocketdb_features (a
// static archive must never need a symbol from an archive that depends on
// it -- see the WAL/changefeed comments in the top-level CMakeLists.txt for
// the two cases that instead had to move the other direction). Only these
// three methods lived in environment.cpp; Environment::blobStore() is
// unaffected -- it was already inline in environment.hpp.
//
#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/blob/blob_store.hpp"

namespace annpocketdb::api {

Result<blob::BlobRef> Environment::putBlob(ByteView key, ByteView data, core::SyncMode mode) {
#if !defined(ANNPOCKETDB_WITH_BLOB)
    (void)key; (void)data; (void)mode;
    return Status::NotSupported("blob support was not built (ANNPOCKETDB_WITH_BLOB)");
#else
    auto wt = beginWrite();
    if (!wt) return wt.status();
    txn::WriteTransaction wtxn = wt.moveValue();

    // Replace any previous blob chain bound to this key.
    auto prev = wtxn.get(key);
    if (prev.ok() && blob::isBlobRef(toByteView(prev.value()))) {
        blob::BlobRef oldRef;
        if (blob::decodeBlobRef(toByteView(prev.value()), oldRef).ok()) {
            blob::BlobStore store(*pm_);
            Status rs = store.remove(oldRef.id, wtxn.txnId());
            if (!rs.ok()) {
                wtxn.abort();
                return rs;
            }
        }
    }

    blob::BlobStore store(*pm_);
    auto refR = store.put(data, wtxn.txnId());
    if (!refR) {
        wtxn.abort();
        return refR.status();
    }
    ByteBuffer refVal = blob::encodeBlobRef(refR.value());
    Status s = wtxn.put(key, toByteView(refVal), true);
    if (!s.ok()) {
        wtxn.abort();
        return s;
    }
    s = wtxn.commit(mode);
    if (!s.ok()) return s;
    return Result<blob::BlobRef>(refR.value());
#endif
}

Result<ByteBuffer> Environment::getBlob(ByteView key) {
#if !defined(ANNPOCKETDB_WITH_BLOB)
    return get(key);  // no blob refs can exist without ANNPOCKETDB_WITH_BLOB (putBlob is NotSupported)
#else
    auto g = get(key);
    if (!g) return g.status();
    ByteView val = toByteView(g.value());
    if (!blob::isBlobRef(val)) {
        // Inline value — return as-is for convenience.
        return Result<ByteBuffer>(g.moveValue());
    }
    blob::BlobRef ref;
    Status s = blob::decodeBlobRef(val, ref);
    if (!s.ok()) return s;
    blob::BlobStore store(*pm_);
    return store.get(ref.id);
#endif
}

Status Environment::removeBlob(ByteView key, core::SyncMode mode) {
#if !defined(ANNPOCKETDB_WITH_BLOB)
    return remove(key, mode);  // no blob refs can exist without ANNPOCKETDB_WITH_BLOB (putBlob is NotSupported)
#else
    auto wt = beginWrite();
    if (!wt) return wt.status();
    txn::WriteTransaction wtxn = wt.moveValue();

    auto prev = wtxn.get(key);
    if (!prev) {
        wtxn.abort();
        return prev.status();
    }
    if (blob::isBlobRef(toByteView(prev.value()))) {
        blob::BlobRef ref;
        Status s = blob::decodeBlobRef(toByteView(prev.value()), ref);
        if (!s.ok()) {
            wtxn.abort();
            return s;
        }
        blob::BlobStore store(*pm_);
        s = store.remove(ref.id, wtxn.txnId());
        if (!s.ok()) {
            wtxn.abort();
            return s;
        }
    }
    Status s = wtxn.remove(key);
    if (!s.ok()) {
        wtxn.abort();
        return s;
    }
    return wtxn.commit(mode);
#endif
}

}  // namespace annpocketdb::api
