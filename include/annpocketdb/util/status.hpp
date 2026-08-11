#pragma once
//
// AnnPocketDb :: util/status.hpp
//
// Lightweight, allocation-conscious error handling used across every layer
// of the engine. Exceptions are avoided on hot paths (page IO, btree
// traversal, cursor movement) because embedded storage engines need
// predictable latency; exceptions are reserved for rare setup errors.
//
#include <string>
#include <utility>
#include <variant>
#include <cassert>

namespace annpocketdb {

enum class StatusCode {
    Ok = 0, NotFound, Corruption, IOError, InvalidArgument, KeyExists,
    TxnConflict, ReadOnly, MapFull, Incompatible, Busy, NotSupported, Internal,
    // ---- LMDB-parity granular codes (see util/status.hpp's factory methods
    // below for what each maps to and README/API docs for the LMDB error
    // class each mirrors). Added for callers that want to distinguish
    // failure modes LMDB itself distinguishes, rather than lumping them
    // into the generic codes above (which remain valid and are still used
    // wherever a more specific code doesn't apply). ----
    PageNotFound,    // LMDB PageNotFoundError: a specific physical page id is absent
    Panic,           // LMDB PanicError: a meta-page update itself failed
    VersionMismatch, // LMDB VersionMismatchError: on-disk format version unsupported
    InvalidFile,     // LMDB InvalidError: not a valid database file at all
    DbsFull,         // LMDB DbsFullError: maxNamedDatabases limit reached
    ReadersFull,     // LMDB ReadersFullError: maxReaders limit reached
    MapResized,      // LMDB MapResizedError: another process grew the map
    BadDbi,          // LMDB BadDbiError: sub-database handle changed underneath the caller
    BadTxn,          // LMDB BadTxnError: transaction cannot recover, must be aborted
    BadValSize,      // LMDB BadValsizeError: oversized/empty key, or DUPFIXED size mismatch
    Locked,          // LMDB "environment locked by another process"
    OutOfMemory,     // LMDB MemoryError
    DiskFull,        // LMDB DiskError: no more disk space
};

class Status {
public:
    Status() = default;
    static Status OK() { return Status(); }
    static Status Error(StatusCode code, std::string message) {
        Status s; s.code_ = code; s.message_ = std::move(message); return s;
    }
    static Status NotFound(std::string msg = "not found") { return Error(StatusCode::NotFound, std::move(msg)); }
    static Status Corruption(std::string msg) { return Error(StatusCode::Corruption, std::move(msg)); }
    static Status IOError(std::string msg) { return Error(StatusCode::IOError, std::move(msg)); }
    static Status InvalidArgument(std::string msg) { return Error(StatusCode::InvalidArgument, std::move(msg)); }
    static Status KeyExists(std::string msg = "key already exists") { return Error(StatusCode::KeyExists, std::move(msg)); }
    static Status TxnConflict(std::string msg = "transaction conflict") { return Error(StatusCode::TxnConflict, std::move(msg)); }
    static Status ReadOnly(std::string msg = "transaction is read-only") { return Error(StatusCode::ReadOnly, std::move(msg)); }
    static Status MapFull(std::string msg = "map size limit reached") { return Error(StatusCode::MapFull, std::move(msg)); }
    static Status Incompatible(std::string msg) { return Error(StatusCode::Incompatible, std::move(msg)); }
    static Status Busy(std::string msg = "resource busy") { return Error(StatusCode::Busy, std::move(msg)); }
    static Status NotSupported(std::string msg) { return Error(StatusCode::NotSupported, std::move(msg)); }
    static Status Internal(std::string msg) { return Error(StatusCode::Internal, std::move(msg)); }

    static Status PageNotFound(std::string msg = "requested page not found") {
        return Error(StatusCode::PageNotFound, std::move(msg));
    }
    static Status Panic(std::string msg = "update of meta page failed") {
        return Error(StatusCode::Panic, std::move(msg));
    }
    static Status VersionMismatch(std::string msg = "database environment version mismatch") {
        return Error(StatusCode::VersionMismatch, std::move(msg));
    }
    static Status InvalidFile(std::string msg = "file is not a valid AnnPocketDb database") {
        return Error(StatusCode::InvalidFile, std::move(msg));
    }
    static Status DbsFull(std::string msg = "maxNamedDatabases limit reached") {
        return Error(StatusCode::DbsFull, std::move(msg));
    }
    static Status ReadersFull(std::string msg = "maxReaders limit reached") {
        return Error(StatusCode::ReadersFull, std::move(msg));
    }
    static Status MapResized(std::string msg = "database contents grew beyond this process's map size") {
        return Error(StatusCode::MapResized, std::move(msg));
    }
    static Status BadDbi(std::string msg = "sub-database handle changed unexpectedly") {
        return Error(StatusCode::BadDbi, std::move(msg));
    }
    static Status BadTxn(std::string msg = "transaction cannot recover; it must be aborted") {
        return Error(StatusCode::BadTxn, std::move(msg));
    }
    static Status BadValSize(std::string msg = "key/value too large, key empty, or DUPFIXED size mismatch") {
        return Error(StatusCode::BadValSize, std::move(msg));
    }
    static Status Locked(std::string msg = "environment locked by another process") {
        return Error(StatusCode::Locked, std::move(msg));
    }
    static Status OutOfMemory(std::string msg = "out of memory") {
        return Error(StatusCode::OutOfMemory, std::move(msg));
    }
    static Status DiskFull(std::string msg = "no more disk space") {
        return Error(StatusCode::DiskFull, std::move(msg));
    }

    bool ok() const { return code_ == StatusCode::Ok; }
    explicit operator bool() const { return ok(); }
    StatusCode code() const { return code_; }
    const std::string& message() const { return message_; }

    std::string toString() const {
        if (ok()) return "OK";
        if (message_.empty()) return std::string(codeName());
        return std::string(codeName()) + ": " + message_;
    }

private:
    const char* codeName() const {
        switch (code_) {
            case StatusCode::Ok: return "OK";
            case StatusCode::NotFound: return "NotFound";
            case StatusCode::Corruption: return "Corruption";
            case StatusCode::IOError: return "IOError";
            case StatusCode::InvalidArgument: return "InvalidArgument";
            case StatusCode::KeyExists: return "KeyExists";
            case StatusCode::TxnConflict: return "TxnConflict";
            case StatusCode::ReadOnly: return "ReadOnly";
            case StatusCode::MapFull: return "MapFull";
            case StatusCode::Incompatible: return "Incompatible";
            case StatusCode::Busy: return "Busy";
            case StatusCode::NotSupported: return "NotSupported";
            case StatusCode::Internal: return "Internal";
            case StatusCode::PageNotFound: return "PageNotFound";
            case StatusCode::Panic: return "Panic";
            case StatusCode::VersionMismatch: return "VersionMismatch";
            case StatusCode::InvalidFile: return "InvalidFile";
            case StatusCode::DbsFull: return "DbsFull";
            case StatusCode::ReadersFull: return "ReadersFull";
            case StatusCode::MapResized: return "MapResized";
            case StatusCode::BadDbi: return "BadDbi";
            case StatusCode::BadTxn: return "BadTxn";
            case StatusCode::BadValSize: return "BadValSize";
            case StatusCode::Locked: return "Locked";
            case StatusCode::OutOfMemory: return "OutOfMemory";
            case StatusCode::DiskFull: return "DiskFull";
        }
        return "Unknown";
    }
    StatusCode code_ = StatusCode::Ok;
    std::string message_;
};

template <typename T>
class Result {
public:
    Result(T value) : storage_(std::move(value)) {}
    Result(Status status) : storage_(std::move(status)) {
        assert(!std::get<Status>(storage_).ok() && "Result constructed with an Ok status but no value");
    }
    bool ok() const { return std::holds_alternative<T>(storage_); }
    explicit operator bool() const { return ok(); }
    T& value() { return std::get<T>(storage_); }
    const T& value() const { return std::get<T>(storage_); }
    T&& moveValue() { return std::move(std::get<T>(storage_)); }
    const Status& status() const {
        static const Status kOk = Status::OK();
        return ok() ? kOk : std::get<Status>(storage_);
    }
    T& operator*() { return value(); }
    const T& operator*() const { return value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
private:
    std::variant<T, Status> storage_;
};

}  // namespace annpocketdb
