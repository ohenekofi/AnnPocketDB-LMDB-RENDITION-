#pragma once
//
// AnnPocketDb :: document/document.hpp
//
// Document storage on top of the KV layer.
//
// Provides:
//   - Typed Value tree (null/bool/int/string/bytes/array/map)
//   - MessagePack encode/decode (self-contained, no third-party required)
//   - Nested field-path extraction ("user.address.city", "tags.0")
//   - Indexing hooks: extract canonical index bytes for a field path
//   - CRUD helpers over WriteTransaction / ReadTransaction
//
// PlainTextCodec remains available for tests and migration.
//
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "annpocketdb/txn/transaction.hpp"

namespace annpocketdb::index { class IndexManager; }
#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

namespace annpocketdb::document {

// ---------------------------------------------------------------------------
// Typed value model
// ---------------------------------------------------------------------------

enum class ValueType : std::uint8_t {
    Null = 0,
    Bool,
    Int,      // signed
    UInt,     // unsigned
    Float,
    String,
    Bytes,
    Array,
    Map,
};

class Value {
public:
    using Map = std::map<std::string, Value>;
    using Array = std::vector<Value>;

    Value() : type_(ValueType::Null) {}
    static Value null() { return Value(); }
    static Value boolean(bool v) { Value x; x.type_ = ValueType::Bool; x.bool_ = v; return x; }
    static Value integer(std::int64_t v) { Value x; x.type_ = ValueType::Int; x.i64_ = v; return x; }
    static Value uinteger(std::uint64_t v) { Value x; x.type_ = ValueType::UInt; x.u64_ = v; return x; }
    static Value floating(double v) { Value x; x.type_ = ValueType::Float; x.f64_ = v; return x; }
    static Value string(std::string v) {
        Value x; x.type_ = ValueType::String; x.str_ = std::move(v); return x;
    }
    static Value bytes(ByteBuffer v) {
        Value x; x.type_ = ValueType::Bytes; x.bytes_ = std::move(v); return x;
    }
    static Value array(Array v) {
        Value x; x.type_ = ValueType::Array; x.arr_ = std::move(v); return x;
    }
    static Value map(Map v) {
        Value x; x.type_ = ValueType::Map; x.map_ = std::move(v); return x;
    }

    ValueType type() const { return type_; }
    bool isNull() const { return type_ == ValueType::Null; }
    bool isBool() const { return type_ == ValueType::Bool; }
    bool isInt() const { return type_ == ValueType::Int; }
    bool isUInt() const { return type_ == ValueType::UInt; }
    bool isNumber() const {
        return type_ == ValueType::Int || type_ == ValueType::UInt || type_ == ValueType::Float;
    }
    bool isString() const { return type_ == ValueType::String; }
    bool isBytes() const { return type_ == ValueType::Bytes; }
    bool isArray() const { return type_ == ValueType::Array; }
    bool isMap() const { return type_ == ValueType::Map; }

    bool asBool() const { return bool_; }
    std::int64_t asInt() const { return i64_; }
    std::uint64_t asUInt() const { return u64_; }
    double asFloat() const { return f64_; }
    const std::string& asString() const { return str_; }
    const ByteBuffer& asBytes() const { return bytes_; }
    const Array& asArray() const { return arr_; }
    const Map& asMap() const { return map_; }
    Array& asArray() { return arr_; }
    Map& asMap() { return map_; }

    // Canonical string form for predicates / debugging.
    std::string toCanonicalString() const;

    // Canonical bytes for secondary-index keys (order-friendly where possible).
    ByteBuffer toIndexBytes() const;

private:
    ValueType type_ = ValueType::Null;
    bool bool_ = false;
    std::int64_t i64_ = 0;
    std::uint64_t u64_ = 0;
    double f64_ = 0;
    std::string str_;
    ByteBuffer bytes_;
    Array arr_;
    Map map_;
};

// Opaque stored document (raw MessagePack or other codec bytes).
class Document {
public:
    Document() = default;
    explicit Document(ByteBuffer data) : data_(std::move(data)) {}

    ByteView raw() const { return ByteView(data_.data(), data_.size()); }
    ByteBuffer& mutableRaw() { return data_; }
    const ByteBuffer& rawBuffer() const { return data_; }

private:
    ByteBuffer data_;
};

// Field path: "user.name" -> {"user","name"}; "tags.0" -> {"tags","0"}.
using FieldPath = std::vector<std::string>;

// Parse "a.b.0.c" into segments. Empty input => empty path.
FieldPath parseFieldPath(std::string_view dotted);

// Walk a Value tree along path. Returns NotFound if any segment is missing
// or type-incompatible (e.g. indexing into a string).
Status getByPath(const Value& root, const FieldPath& path, Value& out);

// ---------------------------------------------------------------------------
// Codec interface
// ---------------------------------------------------------------------------

class IDocumentCodec {
public:
    virtual ~IDocumentCodec() = default;

    // Structured encode/decode (preferred).
    virtual Status encodeValue(const Value& value, ByteBuffer& out) = 0;
    virtual Status decodeValue(ByteView in, Value& out) = 0;

    // Flat string-map helpers (compatibility with older call sites / tests).
    virtual Status encode(const std::vector<std::pair<std::string, std::string>>& fields,
                          ByteBuffer& out);
    virtual Status decode(ByteView in,
                          std::vector<std::pair<std::string, std::string>>& out);

    // Extract a field by path into its canonical string form.
    virtual Status extractField(ByteView doc, const FieldPath& path, std::string& out);

    // Extract a field as typed Value.
    virtual Status extractValue(ByteView doc, const FieldPath& path, Value& out);

    // Indexing hook: bytes to store in a secondary index for this field path.
    // Returns NotFound if the path is absent (caller may skip indexing).
    virtual Status extractIndexKey(ByteView doc, const FieldPath& path, ByteBuffer& out);
};

// MessagePack codec (self-contained implementation).
class MsgPackCodec : public IDocumentCodec {
public:
    Status encodeValue(const Value& value, ByteBuffer& out) override;
    Status decodeValue(ByteView in, Value& out) override;
};

// Legacy text codec: key=value lines. Maps are flat; path "a.b" looks up key "a.b".
class PlainTextCodec : public IDocumentCodec {
public:
    Status encodeValue(const Value& value, ByteBuffer& out) override;
    Status decodeValue(ByteView in, Value& out) override;
    Status encode(const std::vector<std::pair<std::string, std::string>>& fields,
                  ByteBuffer& out) override;
    Status decode(ByteView in,
                  std::vector<std::pair<std::string, std::string>>& out) override;
    Status extractField(ByteView doc, const FieldPath& path, std::string& out) override;
};

// Default: MsgPackCodec. Pass kind if needed later.
Result<std::unique_ptr<IDocumentCodec>> createDocumentCodec();
Result<std::unique_ptr<IDocumentCodec>> createMsgPackCodec();
Result<std::unique_ptr<IDocumentCodec>> createPlainTextCodec();

// CRUD helpers
Status putDocument(txn::WriteTransaction& txn, ByteView primaryKey, const Document& doc);
Result<Document> getDocument(txn::ReadTransaction& txn, ByteView primaryKey);
Status removeDocument(txn::WriteTransaction& txn, ByteView primaryKey);

// Encode Value -> Document via codec.
Result<Document> encodeDocument(IDocumentCodec& codec, const Value& value);
Result<Value> decodeDocument(IDocumentCodec& codec, const Document& doc);

// Build secondary-index entries for a document: list of (fieldPath, indexBytes).
// `indexedPaths` is the set of paths the caller maintains indexes for.
Status collectIndexEntries(IDocumentCodec& codec,
                           ByteView docBytes,
                           const std::vector<FieldPath>& indexedPaths,
                           std::vector<std::pair<FieldPath, ByteBuffer>>& out);

// Extract all index keys for a path. If the path ends at an array, one key
// is produced per element (multikey index). Otherwise 0 or 1 key.
Status extractIndexKeys(IDocumentCodec& codec, ByteView doc, const FieldPath& path,
                        std::vector<ByteBuffer>& out);

// ---------------------------------------------------------------------------
// DocumentStore — high-level document API with optional automatic index maintenance
// ---------------------------------------------------------------------------
//
// When an IndexManager is attached, put/remove update every registered index
// via maintainOnPut / maintainOnRemove and saveCatalog. Callers must still
// commit the WriteTransaction.
//
class DocumentStore {
public:
    DocumentStore(IDocumentCodec& codec, index::IndexManager* indexes = nullptr);

    void setIndexManager(index::IndexManager* indexes) { indexes_ = indexes; }
    index::IndexManager* indexManager() const { return indexes_; }
    IDocumentCodec& codec() { return *codec_; }

    // Encode + put; maintains secondary indexes when IndexManager is set.
    Status put(txn::WriteTransaction& txn, ByteView primaryKey, const Value& value);
    Status put(txn::WriteTransaction& txn, ByteView primaryKey, const Document& doc);

    Result<Document> get(txn::ReadTransaction& txn, ByteView primaryKey) const;
    Result<Value> getValue(txn::ReadTransaction& txn, ByteView primaryKey) const;

    // Removes document and drops secondary index entries.
    Status remove(txn::WriteTransaction& txn, ByteView primaryKey);

    // Ensure catalog is persisted (call after put/remove when indexes attached).
    Status saveIndexes(txn::WriteTransaction& txn);

private:
    IDocumentCodec* codec_ = nullptr;
    index::IndexManager* indexes_ = nullptr;
};



}  // namespace annpocketdb::document
