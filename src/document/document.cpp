#include "annpocketdb/document/document.hpp"
#include "annpocketdb/index/index.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace annpocketdb::document {

// ---------------------------------------------------------------------------
// Value helpers
// ---------------------------------------------------------------------------

std::string Value::toCanonicalString() const {
    switch (type_) {
        case ValueType::Null: return "";
        case ValueType::Bool: return bool_ ? "true" : "false";
        case ValueType::Int: return std::to_string(i64_);
        case ValueType::UInt: return std::to_string(u64_);
        case ValueType::Float: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g", f64_);
            return std::string(buf);
        }
        case ValueType::String: return str_;
        case ValueType::Bytes: return std::string(reinterpret_cast<const char*>(bytes_.data()), bytes_.size());
        case ValueType::Array: return "[array:" + std::to_string(arr_.size()) + "]";
        case ValueType::Map: return "[map:" + std::to_string(map_.size()) + "]";
    }
    return "";
}

ByteBuffer Value::toIndexBytes() const {
    // Prefix with type tag so different types don't collide in the index.
    ByteBuffer out;
    auto append = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const std::byte*>(p);
        out.insert(out.end(), b, b + n);
    };
    auto appendU8 = [&](std::uint8_t v) { out.push_back(static_cast<std::byte>(v)); };

    switch (type_) {
        case ValueType::Null:
            appendU8(0);
            break;
        case ValueType::Bool:
            appendU8(1);
            appendU8(bool_ ? 1 : 0);
            break;
        case ValueType::Int: {
            appendU8(2);
            // Big-endian int64 so byte-wise order matches numeric order for non-negatives;
            // full signed order is not required for equality indexes.
            std::uint64_t u = static_cast<std::uint64_t>(i64_);
            for (int i = 7; i >= 0; --i) appendU8(static_cast<std::uint8_t>((u >> (i * 8)) & 0xFF));
            break;
        }
        case ValueType::UInt: {
            appendU8(3);
            for (int i = 7; i >= 0; --i) appendU8(static_cast<std::uint8_t>((u64_ >> (i * 8)) & 0xFF));
            break;
        }
        case ValueType::Float: {
            appendU8(4);
            std::uint64_t bits = 0;
            static_assert(sizeof(double) == 8, "double must be 8 bytes");
            std::memcpy(&bits, &f64_, 8);
            for (int i = 7; i >= 0; --i) appendU8(static_cast<std::uint8_t>((bits >> (i * 8)) & 0xFF));
            break;
        }
        case ValueType::String:
            appendU8(5);
            append(str_.data(), str_.size());
            break;
        case ValueType::Bytes:
            appendU8(6);
            append(bytes_.data(), bytes_.size());
            break;
        case ValueType::Array:
        case ValueType::Map:
            // Composite values: index the canonical string form.
            appendU8(7);
            {
                std::string s = toCanonicalString();
                append(s.data(), s.size());
            }
            break;
    }
    return out;
}

FieldPath parseFieldPath(std::string_view dotted) {
    FieldPath path;
    if (dotted.empty()) return path;
    std::size_t start = 0;
    while (start <= dotted.size()) {
        std::size_t dot = dotted.find('.', start);
        if (dot == std::string_view::npos) {
            path.emplace_back(dotted.substr(start));
            break;
        }
        path.emplace_back(dotted.substr(start, dot - start));
        start = dot + 1;
    }
    path.erase(std::remove_if(path.begin(), path.end(),
                              [](const std::string& s) { return s.empty(); }),
               path.end());
    return path;
}

Status getByPath(const Value& root, const FieldPath& path, Value& out) {
    const Value* cur = &root;
    for (const std::string& seg : path) {
        if (cur->isMap()) {
            const auto& m = cur->asMap();
            auto it = m.find(seg);
            if (it == m.end()) return Status::NotFound("field not found: " + seg);
            cur = &it->second;
        } else if (cur->isArray()) {
            // Numeric index into array.
            char* end = nullptr;
            long idx = std::strtol(seg.c_str(), &end, 10);
            if (end == seg.c_str() || *end != '\0' || idx < 0) {
                return Status::NotFound("invalid array index: " + seg);
            }
            const auto& a = cur->asArray();
            if (static_cast<std::size_t>(idx) >= a.size()) {
                return Status::NotFound("array index out of range: " + seg);
            }
            cur = &a[static_cast<std::size_t>(idx)];
        } else {
            return Status::NotFound("cannot descend into non-container at: " + seg);
        }
    }
    out = *cur;
    return Status::OK();
}

// ---------------------------------------------------------------------------
// Default IDocumentCodec methods
// ---------------------------------------------------------------------------

Status IDocumentCodec::encode(const std::vector<std::pair<std::string, std::string>>& fields,
                               ByteBuffer& out) {
    Value::Map m;
    for (const auto& [k, v] : fields) m.emplace(k, Value::string(v));
    return encodeValue(Value::map(std::move(m)), out);
}

Status IDocumentCodec::decode(ByteView in,
                               std::vector<std::pair<std::string, std::string>>& out) {
    Value v;
    Status s = decodeValue(in, v);
    if (!s.ok()) return s;
    out.clear();
    if (!v.isMap()) return Status::Corruption("document root is not a map");
    for (const auto& [k, val] : v.asMap()) {
        out.emplace_back(k, val.toCanonicalString());
    }
    return Status::OK();
}

Status IDocumentCodec::extractField(ByteView doc, const FieldPath& path, std::string& out) {
    Value v;
    Status s = extractValue(doc, path, v);
    if (!s.ok()) return s;
    out = v.toCanonicalString();
    return Status::OK();
}

Status IDocumentCodec::extractValue(ByteView doc, const FieldPath& path, Value& out) {
    Value root;
    Status s = decodeValue(doc, root);
    if (!s.ok()) return s;
    if (path.empty()) {
        out = std::move(root);
        return Status::OK();
    }
    return getByPath(root, path, out);
}

Status IDocumentCodec::extractIndexKey(ByteView doc, const FieldPath& path, ByteBuffer& out) {
    Value v;
    Status s = extractValue(doc, path, v);
    if (!s.ok()) return s;
    out = v.toIndexBytes();
    return Status::OK();
}

// ---------------------------------------------------------------------------
// MessagePack (subset) — encode
// ---------------------------------------------------------------------------

namespace msgpack {
namespace {

void writeU8(ByteBuffer& o, std::uint8_t v) { o.push_back(static_cast<std::byte>(v)); }
void writeU16(ByteBuffer& o, std::uint16_t v) {
    writeU8(o, static_cast<std::uint8_t>(v >> 8));
    writeU8(o, static_cast<std::uint8_t>(v));
}
void writeU32(ByteBuffer& o, std::uint32_t v) {
    writeU8(o, static_cast<std::uint8_t>(v >> 24));
    writeU8(o, static_cast<std::uint8_t>(v >> 16));
    writeU8(o, static_cast<std::uint8_t>(v >> 8));
    writeU8(o, static_cast<std::uint8_t>(v));
}
void writeU64(ByteBuffer& o, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) writeU8(o, static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF));
}

Status encode(const Value& v, ByteBuffer& o);

Status encodeString(const std::string& s, ByteBuffer& o) {
    const auto n = s.size();
    if (n <= 31) {
        writeU8(o, static_cast<std::uint8_t>(0xa0 | n));
    } else if (n <= 0xFF) {
        writeU8(o, 0xd9);
        writeU8(o, static_cast<std::uint8_t>(n));
    } else if (n <= 0xFFFF) {
        writeU8(o, 0xda);
        writeU16(o, static_cast<std::uint16_t>(n));
    } else {
        writeU8(o, 0xdb);
        writeU32(o, static_cast<std::uint32_t>(n));
    }
    o.insert(o.end(), reinterpret_cast<const std::byte*>(s.data()),
             reinterpret_cast<const std::byte*>(s.data()) + n);
    return Status::OK();
}

Status encode(const Value& v, ByteBuffer& o) {
    switch (v.type()) {
        case ValueType::Null:
            writeU8(o, 0xc0);
            return Status::OK();
        case ValueType::Bool:
            writeU8(o, v.asBool() ? 0xc3 : 0xc2);
            return Status::OK();
        case ValueType::Int: {
            std::int64_t i = v.asInt();
            if (i >= 0) {
                // positive as unsigned family for compact form
                std::uint64_t u = static_cast<std::uint64_t>(i);
                if (u <= 0x7f) {
                    writeU8(o, static_cast<std::uint8_t>(u));
                } else if (u <= 0xFF) {
                    writeU8(o, 0xcc); writeU8(o, static_cast<std::uint8_t>(u));
                } else if (u <= 0xFFFF) {
                    writeU8(o, 0xcd); writeU16(o, static_cast<std::uint16_t>(u));
                } else if (u <= 0xFFFFFFFFu) {
                    writeU8(o, 0xce); writeU32(o, static_cast<std::uint32_t>(u));
                } else {
                    writeU8(o, 0xcf); writeU64(o, u);
                }
            } else if (i >= -32) {
                writeU8(o, static_cast<std::uint8_t>(static_cast<std::int8_t>(i)));
            } else if (i >= std::numeric_limits<std::int8_t>::min()) {
                writeU8(o, 0xd0); writeU8(o, static_cast<std::uint8_t>(static_cast<std::int8_t>(i)));
            } else if (i >= std::numeric_limits<std::int16_t>::min()) {
                writeU8(o, 0xd1); writeU16(o, static_cast<std::uint16_t>(static_cast<std::int16_t>(i)));
            } else if (i >= std::numeric_limits<std::int32_t>::min()) {
                writeU8(o, 0xd2); writeU32(o, static_cast<std::uint32_t>(static_cast<std::int32_t>(i)));
            } else {
                writeU8(o, 0xd3); writeU64(o, static_cast<std::uint64_t>(i));
            }
            return Status::OK();
        }
        case ValueType::UInt: {
            std::uint64_t u = v.asUInt();
            if (u <= 0x7f) {
                writeU8(o, static_cast<std::uint8_t>(u));
            } else if (u <= 0xFF) {
                writeU8(o, 0xcc); writeU8(o, static_cast<std::uint8_t>(u));
            } else if (u <= 0xFFFF) {
                writeU8(o, 0xcd); writeU16(o, static_cast<std::uint16_t>(u));
            } else if (u <= 0xFFFFFFFFu) {
                writeU8(o, 0xce); writeU32(o, static_cast<std::uint32_t>(u));
            } else {
                writeU8(o, 0xcf); writeU64(o, u);
            }
            return Status::OK();
        }
        case ValueType::Float: {
            writeU8(o, 0xcb);  // float64
            double d = v.asFloat();
            std::uint64_t bits = 0;
            std::memcpy(&bits, &d, 8);
            writeU64(o, bits);
            return Status::OK();
        }
        case ValueType::String:
            return encodeString(v.asString(), o);
        case ValueType::Bytes: {
            const auto& b = v.asBytes();
            const auto n = b.size();
            if (n <= 0xFF) {
                writeU8(o, 0xc4); writeU8(o, static_cast<std::uint8_t>(n));
            } else if (n <= 0xFFFF) {
                writeU8(o, 0xc5); writeU16(o, static_cast<std::uint16_t>(n));
            } else {
                writeU8(o, 0xc6); writeU32(o, static_cast<std::uint32_t>(n));
            }
            o.insert(o.end(), b.begin(), b.end());
            return Status::OK();
        }
        case ValueType::Array: {
            const auto& a = v.asArray();
            const auto n = a.size();
            if (n <= 15) {
                writeU8(o, static_cast<std::uint8_t>(0x90 | n));
            } else if (n <= 0xFFFF) {
                writeU8(o, 0xdc); writeU16(o, static_cast<std::uint16_t>(n));
            } else {
                writeU8(o, 0xdd); writeU32(o, static_cast<std::uint32_t>(n));
            }
            for (const auto& el : a) {
                Status s = encode(el, o);
                if (!s.ok()) return s;
            }
            return Status::OK();
        }
        case ValueType::Map: {
            const auto& m = v.asMap();
            const auto n = m.size();
            if (n <= 15) {
                writeU8(o, static_cast<std::uint8_t>(0x80 | n));
            } else if (n <= 0xFFFF) {
                writeU8(o, 0xde); writeU16(o, static_cast<std::uint16_t>(n));
            } else {
                writeU8(o, 0xdf); writeU32(o, static_cast<std::uint32_t>(n));
            }
            for (const auto& [k, val] : m) {
                Status s = encodeString(k, o);
                if (!s.ok()) return s;
                s = encode(val, o);
                if (!s.ok()) return s;
            }
            return Status::OK();
        }
    }
    return Status::Internal("unknown value type");
}

// ---------------------------------------------------------------------------
// MessagePack — decode
// ---------------------------------------------------------------------------

struct Reader {
    const std::byte* p = nullptr;
    const std::byte* end = nullptr;

    bool empty() const { return p >= end; }
    Status need(std::size_t n) const {
        if (static_cast<std::size_t>(end - p) < n) return Status::Corruption("msgpack truncated");
        return Status::OK();
    }
    Status readU8(std::uint8_t& v) {
        Status s = need(1); if (!s.ok()) return s;
        v = static_cast<std::uint8_t>(*p++);
        return Status::OK();
    }
    Status readU16(std::uint16_t& v) {
        Status s = need(2); if (!s.ok()) return s;
        v = (static_cast<std::uint16_t>(static_cast<std::uint8_t>(p[0])) << 8) |
            static_cast<std::uint16_t>(static_cast<std::uint8_t>(p[1]));
        p += 2;
        return Status::OK();
    }
    Status readU32(std::uint32_t& v) {
        Status s = need(4); if (!s.ok()) return s;
        v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | static_cast<std::uint8_t>(p[i]);
        p += 4;
        return Status::OK();
    }
    Status readU64(std::uint64_t& v) {
        Status s = need(8); if (!s.ok()) return s;
        v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<std::uint8_t>(p[i]);
        p += 8;
        return Status::OK();
    }
    Status readBytes(std::size_t n, ByteBuffer& out) {
        Status s = need(n); if (!s.ok()) return s;
        out.assign(p, p + n);
        p += n;
        return Status::OK();
    }
    Status readString(std::size_t n, std::string& out) {
        Status s = need(n); if (!s.ok()) return s;
        out.assign(reinterpret_cast<const char*>(p), n);
        p += n;
        return Status::OK();
    }
};

Status decode(Reader& r, Value& out);

Status decodeArray(Reader& r, std::size_t n, Value& out) {
    Value::Array a;
    a.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        Value el;
        Status s = decode(r, el);
        if (!s.ok()) return s;
        a.push_back(std::move(el));
    }
    out = Value::array(std::move(a));
    return Status::OK();
}

Status decodeMap(Reader& r, std::size_t n, Value& out) {
    Value::Map m;
    for (std::size_t i = 0; i < n; ++i) {
        Value keyV;
        Status s = decode(r, keyV);
        if (!s.ok()) return s;
        if (!keyV.isString()) return Status::Corruption("msgpack map key is not a string");
        Value val;
        s = decode(r, val);
        if (!s.ok()) return s;
        m.emplace(keyV.asString(), std::move(val));
    }
    out = Value::map(std::move(m));
    return Status::OK();
}

Status decode(Reader& r, Value& out) {
    std::uint8_t b = 0;
    Status s = r.readU8(b);
    if (!s.ok()) return s;

    // positive fixint
    if (b <= 0x7f) {
        out = Value::uinteger(b);
        return Status::OK();
    }
    // fixmap
    if ((b & 0xf0) == 0x80) return decodeMap(r, b & 0x0f, out);
    // fixarray
    if ((b & 0xf0) == 0x90) return decodeArray(r, b & 0x0f, out);
    // fixstr
    if ((b & 0xe0) == 0xa0) {
        std::string str;
        s = r.readString(b & 0x1f, str);
        if (!s.ok()) return s;
        out = Value::string(std::move(str));
        return Status::OK();
    }
    // negative fixint
    if (b >= 0xe0) {
        out = Value::integer(static_cast<std::int8_t>(b));
        return Status::OK();
    }

    switch (b) {
        case 0xc0: out = Value::null(); return Status::OK();
        case 0xc2: out = Value::boolean(false); return Status::OK();
        case 0xc3: out = Value::boolean(true); return Status::OK();
        case 0xc4: {
            std::uint8_t n; s = r.readU8(n); if (!s.ok()) return s;
            ByteBuffer buf; s = r.readBytes(n, buf); if (!s.ok()) return s;
            out = Value::bytes(std::move(buf)); return Status::OK();
        }
        case 0xc5: {
            std::uint16_t n; s = r.readU16(n); if (!s.ok()) return s;
            ByteBuffer buf; s = r.readBytes(n, buf); if (!s.ok()) return s;
            out = Value::bytes(std::move(buf)); return Status::OK();
        }
        case 0xc6: {
            std::uint32_t n; s = r.readU32(n); if (!s.ok()) return s;
            ByteBuffer buf; s = r.readBytes(n, buf); if (!s.ok()) return s;
            out = Value::bytes(std::move(buf)); return Status::OK();
        }
        case 0xca: {  // float32
            std::uint32_t bits; s = r.readU32(bits); if (!s.ok()) return s;
            float f; std::memcpy(&f, &bits, 4);
            out = Value::floating(static_cast<double>(f)); return Status::OK();
        }
        case 0xcb: {
            std::uint64_t bits; s = r.readU64(bits); if (!s.ok()) return s;
            double d; std::memcpy(&d, &bits, 8);
            out = Value::floating(d); return Status::OK();
        }
        case 0xcc: {
            std::uint8_t n; s = r.readU8(n); if (!s.ok()) return s;
            out = Value::uinteger(n); return Status::OK();
        }
        case 0xcd: {
            std::uint16_t n; s = r.readU16(n); if (!s.ok()) return s;
            out = Value::uinteger(n); return Status::OK();
        }
        case 0xce: {
            std::uint32_t n; s = r.readU32(n); if (!s.ok()) return s;
            out = Value::uinteger(n); return Status::OK();
        }
        case 0xcf: {
            std::uint64_t n; s = r.readU64(n); if (!s.ok()) return s;
            out = Value::uinteger(n); return Status::OK();
        }
        case 0xd0: {
            std::uint8_t n; s = r.readU8(n); if (!s.ok()) return s;
            out = Value::integer(static_cast<std::int8_t>(n)); return Status::OK();
        }
        case 0xd1: {
            std::uint16_t n; s = r.readU16(n); if (!s.ok()) return s;
            out = Value::integer(static_cast<std::int16_t>(n)); return Status::OK();
        }
        case 0xd2: {
            std::uint32_t n; s = r.readU32(n); if (!s.ok()) return s;
            out = Value::integer(static_cast<std::int32_t>(n)); return Status::OK();
        }
        case 0xd3: {
            std::uint64_t n; s = r.readU64(n); if (!s.ok()) return s;
            out = Value::integer(static_cast<std::int64_t>(n)); return Status::OK();
        }
        case 0xd9: {
            std::uint8_t n; s = r.readU8(n); if (!s.ok()) return s;
            std::string str; s = r.readString(n, str); if (!s.ok()) return s;
            out = Value::string(std::move(str)); return Status::OK();
        }
        case 0xda: {
            std::uint16_t n; s = r.readU16(n); if (!s.ok()) return s;
            std::string str; s = r.readString(n, str); if (!s.ok()) return s;
            out = Value::string(std::move(str)); return Status::OK();
        }
        case 0xdb: {
            std::uint32_t n; s = r.readU32(n); if (!s.ok()) return s;
            std::string str; s = r.readString(n, str); if (!s.ok()) return s;
            out = Value::string(std::move(str)); return Status::OK();
        }
        case 0xdc: {
            std::uint16_t n; s = r.readU16(n); if (!s.ok()) return s;
            return decodeArray(r, n, out);
        }
        case 0xdd: {
            std::uint32_t n; s = r.readU32(n); if (!s.ok()) return s;
            return decodeArray(r, n, out);
        }
        case 0xde: {
            std::uint16_t n; s = r.readU16(n); if (!s.ok()) return s;
            return decodeMap(r, n, out);
        }
        case 0xdf: {
            std::uint32_t n; s = r.readU32(n); if (!s.ok()) return s;
            return decodeMap(r, n, out);
        }
        default:
            return Status::Corruption("unsupported msgpack type byte " + std::to_string(b));
    }
}

}  // namespace

Status encodeValue(const Value& value, ByteBuffer& out) {
    out.clear();
    return encode(value, out);
}
Status decodeValue(ByteView in, Value& out) {
    Reader r{in.data(), in.data() + in.size()};
    Status s = decode(r, out);
    if (!s.ok()) return s;
    if (!r.empty()) return Status::Corruption("msgpack trailing bytes");
    return Status::OK();
}

}  // namespace msgpack

// ---------------------------------------------------------------------------
// MsgPackCodec
// ---------------------------------------------------------------------------

Status MsgPackCodec::encodeValue(const Value& value, ByteBuffer& out) {
    return msgpack::encodeValue(value, out);
}
Status MsgPackCodec::decodeValue(ByteView in, Value& out) {
    return msgpack::decodeValue(in, out);
}

// ---------------------------------------------------------------------------
// PlainTextCodec
// ---------------------------------------------------------------------------

Status PlainTextCodec::encodeValue(const Value& value, ByteBuffer& out) {
    if (!value.isMap()) return Status::InvalidArgument("PlainTextCodec only encodes maps");
    std::vector<std::pair<std::string, std::string>> fields;
    for (const auto& [k, v] : value.asMap()) fields.emplace_back(k, v.toCanonicalString());
    return encode(fields, out);
}

Status PlainTextCodec::decodeValue(ByteView in, Value& out) {
    std::vector<std::pair<std::string, std::string>> fields;
    Status s = decode(in, fields);
    if (!s.ok()) return s;
    Value::Map m;
    for (const auto& [k, v] : fields) m.emplace(k, Value::string(v));
    out = Value::map(std::move(m));
    return Status::OK();
}

Status PlainTextCodec::encode(const std::vector<std::pair<std::string, std::string>>& fields,
                               ByteBuffer& out) {
    std::string text;
    for (const auto& [k, v] : fields) text += k + "=" + v + "\n";
    out = toByteBuffer(text);
    return Status::OK();
}

Status PlainTextCodec::decode(ByteView in,
                               std::vector<std::pair<std::string, std::string>>& out) {
    std::string_view sv = toStringView(in);
    out.clear();
    std::size_t pos = 0;
    while (pos < sv.size()) {
        std::size_t end = sv.find('\n', pos);
        if (end == std::string_view::npos) end = sv.size();
        std::string_view line = sv.substr(pos, end - pos);
        std::size_t eq = line.find('=');
        if (eq != std::string_view::npos) {
            out.emplace_back(std::string(line.substr(0, eq)), std::string(line.substr(eq + 1)));
        }
        pos = end + 1;
    }
    return Status::OK();
}

Status PlainTextCodec::extractField(ByteView doc, const FieldPath& path, std::string& out) {
    if (path.empty()) return Status::InvalidArgument("empty field path");
    std::string key = path[0];
    for (std::size_t i = 1; i < path.size(); ++i) key += "." + path[i];
    std::vector<std::pair<std::string, std::string>> fields;
    Status s = decode(doc, fields);
    if (!s.ok()) return s;
    for (const auto& [k, v] : fields) {
        if (k == key) {
            out = v;
            return Status::OK();
        }
    }
    return Status::NotFound("field not found: " + key);
}

// ---------------------------------------------------------------------------
// Factories + CRUD
// ---------------------------------------------------------------------------

Result<std::unique_ptr<IDocumentCodec>> createMsgPackCodec() {
    return Result<std::unique_ptr<IDocumentCodec>>(std::make_unique<MsgPackCodec>());
}
Result<std::unique_ptr<IDocumentCodec>> createPlainTextCodec() {
    return Result<std::unique_ptr<IDocumentCodec>>(std::make_unique<PlainTextCodec>());
}
Result<std::unique_ptr<IDocumentCodec>> createDocumentCodec() {
    return createMsgPackCodec();
}

Status putDocument(txn::WriteTransaction& txn, ByteView primaryKey, const Document& doc) {
    return txn.put(primaryKey, doc.raw(), true);
}
Result<Document> getDocument(txn::ReadTransaction& txn, ByteView primaryKey) {
    auto r = txn.get(primaryKey);
    if (!r) return r.status();
    return Result<Document>(Document(r.moveValue()));
}
Status removeDocument(txn::WriteTransaction& txn, ByteView primaryKey) {
    return txn.remove(primaryKey);
}

Result<Document> encodeDocument(IDocumentCodec& codec, const Value& value) {
    ByteBuffer buf;
    Status s = codec.encodeValue(value, buf);
    if (!s.ok()) return s;
    return Result<Document>(Document(std::move(buf)));
}
Result<Value> decodeDocument(IDocumentCodec& codec, const Document& doc) {
    Value v;
    Status s = codec.decodeValue(doc.raw(), v);
    if (!s.ok()) return s;
    return Result<Value>(std::move(v));
}

Status collectIndexEntries(IDocumentCodec& codec,
                           ByteView docBytes,
                           const std::vector<FieldPath>& indexedPaths,
                           std::vector<std::pair<FieldPath, ByteBuffer>>& out) {
    out.clear();
    for (const auto& path : indexedPaths) {
        ByteBuffer key;
        Status s = codec.extractIndexKey(docBytes, path, key);
        if (!s.ok()) {
            if (s.code() == StatusCode::NotFound) continue;  // sparse index
            return s;
        }
        out.emplace_back(path, std::move(key));
    }
    return Status::OK();
}

Status extractIndexKeys(IDocumentCodec& codec, ByteView doc, const FieldPath& path,
                        std::vector<ByteBuffer>& out) {
    out.clear();
    Value root;
    Status s = codec.decodeValue(doc, root);
    if (!s.ok()) return s;
    if (path.empty()) {
        out.push_back(root.toIndexBytes());
        return Status::OK();
    }
    Value leafVal;
    s = getByPath(root, path, leafVal);
    if (!s.ok()) return s;
    if (leafVal.isArray()) {
        for (const auto& el : leafVal.asArray()) out.push_back(el.toIndexBytes());
        return Status::OK();
    }
    out.push_back(leafVal.toIndexBytes());
    return Status::OK();
}

DocumentStore::DocumentStore(IDocumentCodec& codec, index::IndexManager* indexes)
    : codec_(&codec), indexes_(indexes) {}

Status DocumentStore::put(txn::WriteTransaction& txn, ByteView primaryKey, const Value& value) {
    ByteBuffer buf;
    Status s = codec_->encodeValue(value, buf);
    if (!s.ok()) return s;
    return put(txn, primaryKey, Document(std::move(buf)));
}

Status DocumentStore::put(txn::WriteTransaction& txn, ByteView primaryKey, const Document& doc) {
    ByteBuffer old;
    auto prev = txn.get(primaryKey);
    if (prev.ok()) {
        old = prev.moveValue();
    } else if (prev.status().code() != StatusCode::NotFound) {
        return prev.status();
    }

    Status s = txn.put(primaryKey, doc.raw(), true);
    if (!s.ok()) return s;

    if (indexes_ != nullptr) {
        s = indexes_->maintainOnPut(txn, primaryKey,
                                    ByteView(old.data(), old.size()),
                                    doc.raw(), *codec_);
        if (!s.ok()) return s;
        s = indexes_->saveCatalog(txn);
        if (!s.ok()) return s;
    }
    return Status::OK();
}

Result<Document> DocumentStore::get(txn::ReadTransaction& txn, ByteView primaryKey) const {
    return getDocument(txn, primaryKey);
}

Result<Value> DocumentStore::getValue(txn::ReadTransaction& txn, ByteView primaryKey) const {
    auto d = getDocument(txn, primaryKey);
    if (!d) return d.status();
    Value v;
    Status s = codec_->decodeValue(d.value().raw(), v);
    if (!s.ok()) return s;
    return Result<Value>(std::move(v));
}

Status DocumentStore::remove(txn::WriteTransaction& txn, ByteView primaryKey) {
    auto prev = txn.get(primaryKey);
    if (!prev) return prev.status();
    ByteBuffer old = prev.moveValue();

    if (indexes_ != nullptr) {
        Status s = indexes_->maintainOnRemove(txn, primaryKey,
                                              ByteView(old.data(), old.size()), *codec_);
        if (!s.ok()) return s;
    }

    Status s = txn.remove(primaryKey);
    if (!s.ok()) return s;

    if (indexes_ != nullptr) {
        s = indexes_->saveCatalog(txn);
        if (!s.ok()) return s;
    }
    return Status::OK();
}

Status DocumentStore::saveIndexes(txn::WriteTransaction& txn) {
    if (indexes_ == nullptr) return Status::OK();
    return indexes_->saveCatalog(txn);
}

}  // namespace annpocketdb::document
