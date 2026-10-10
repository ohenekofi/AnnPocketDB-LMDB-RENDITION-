#include "annpocketdb/wal/record.hpp"
#include "annpocketdb/util/crc32c.hpp"
#include <cstring>

namespace annpocketdb::wal {

namespace {
constexpr std::uint32_t kWalMagic = 0x57414C21u; // "WAL!"

void writeU16(std::byte* p, std::uint16_t v) { std::memcpy(p, &v, sizeof(v)); }
std::uint16_t readU16(const std::byte* p) { std::uint16_t v; std::memcpy(&v, p, sizeof(v)); return v; }
void writeU32(std::byte* p, std::uint32_t v) { std::memcpy(p, &v, sizeof(v)); }
std::uint32_t readU32(const std::byte* p) { std::uint32_t v; std::memcpy(&v, p, sizeof(v)); return v; }
void writeU64(std::byte* p, std::uint64_t v) { std::memcpy(p, &v, sizeof(v)); }
std::uint64_t readU64(const std::byte* p) { std::uint64_t v; std::memcpy(&v, p, sizeof(v)); return v; }

std::uint32_t computeRecordCrc(RecordType type, ByteView body) {
    std::uint32_t crc = util::crc32c(0, &kWalMagic, sizeof(kWalMagic));
    std::uint8_t t = static_cast<std::uint8_t>(type);
    crc = util::crc32cExtend(crc, ByteView(reinterpret_cast<const std::byte*>(&t), sizeof(t)));
    crc = util::crc32cExtend(crc, body);
    return crc;
}

Status encodeGeneric(RecordType type, ByteView body, ByteBuffer& out) {
    std::uint32_t crc = computeRecordCrc(type, body);
    std::uint32_t len = static_cast<std::uint32_t>(body.size());
    std::size_t off = out.size();
    out.resize(off + kRecordHeaderBytes + body.size());
    writeU32(out.data() + off, crc);
    writeU32(out.data() + off + 4, len);
    out[off + 4 + 4] = static_cast<std::byte>(type);
    std::memcpy(out.data() + off + 9, body.data(), body.size());
    return Status::OK();
}
}  // namespace

// PageImage (legacy) body:  pageId u64 | type u32 | payloadSize u32 | itemCount u16 | payload
// PageImageV2 body:         same 18-byte prefix | flags u16 | overflowNext u64 | payload
constexpr std::size_t kPageImageV1Prefix = 8 + 4 + 4 + 2;
constexpr std::size_t kPageImageV2Prefix = kPageImageV1Prefix + 2 + 8;

Status encodePageImage(const PageImageRecord& rec, std::uint32_t pageSize, ByteBuffer& out) {
    std::size_t payloadCapacity = pageSize - sizeof(core::PageHeader);
    ByteBuffer body;
    body.reserve(kPageImageV2Prefix + payloadCapacity);
    body.resize(kPageImageV2Prefix);
    std::byte* p = body.data();
    writeU64(p, rec.pageId);
    writeU32(p + 8, static_cast<std::uint32_t>(rec.type));
    writeU32(p + 12, rec.payloadSize);
    writeU16(p + 16, rec.itemCount);
    writeU16(p + 18, rec.flags);
    writeU64(p + 20, rec.overflowNext);
    body.insert(body.end(), rec.payload.begin(), rec.payload.end());
    return encodeGeneric(RecordType::PageImageV2, ByteView(body.data(), body.size()), out);
}

Status encodeFreeListUpdate(const FreeListUpdateRecord& rec, ByteBuffer& out) {
    ByteBuffer body;
    body.resize(8 + 8 + rec.retiredPages.size() * 8);
    std::byte* p = body.data();
    writeU64(p, rec.retiringTxnId);
    writeU64(p + 8, rec.retiredPages.size());
    for (std::size_t i = 0; i < rec.retiredPages.size(); ++i) {
        writeU64(p + 16 + i * 8, rec.retiredPages[i]);
    }
    return encodeGeneric(RecordType::FreeListUpdate, ByteView(body.data(), body.size()), out);
}

Status encodeCommit(const CommitRecord& rec, ByteBuffer& out) {
    ByteBuffer body(40, std::byte{0});
    std::byte* p = body.data();
    writeU64(p, rec.txnId);
    writeU64(p + 8, rec.masterRootPageId);
    writeU64(p + 16, rec.freeListRootPageId);
    writeU64(p + 24, rec.walSequence);
    writeU32(p + 32, rec.pageSize);
    return encodeGeneric(RecordType::Commit, ByteView(body.data(), body.size()), out);
}

Status encodeCheckpoint(const CheckpointRecord& rec, ByteBuffer& out) {
    ByteBuffer body(8, std::byte{0});
    writeU64(body.data(), rec.upToTxnId);
    return encodeGeneric(RecordType::Checkpoint, ByteView(body.data(), body.size()), out);
}

Result<ByteView> validateRecordHeader(ByteView data, RecordType& outType) {
    if (data.size() < 9) return Status::Corruption("WAL record truncated (header)");
    std::uint32_t storedCrc = readU32(data.data());
    std::uint32_t len = readU32(data.data() + 4);
    // Compute bounds in size_t so 9 + len cannot wrap (uint32). A corrupted
    // length near UINT32_MAX must not pass the check and then OOB-read.
    // Hard-cap body size: no legitimate record exceeds a couple of pages
    // (PageImage is the largest); 4 MiB is generous and blocks recovery DoS.
    constexpr std::uint32_t kMaxRecordBodyBytes = 4u << 20;
    if (len > kMaxRecordBodyBytes) {
        return Status::Corruption("WAL record body length exceeds maximum");
    }
    if (len > data.size() - 9) {
        return Status::Corruption("WAL record truncated (body)");
    }
    outType = static_cast<RecordType>(static_cast<std::uint8_t>(data[8]));
    ByteView body(data.data() + 9, len);
    std::uint32_t expected = computeRecordCrc(outType, body);
    if (storedCrc != expected) return Status::Corruption("WAL record CRC mismatch");
    return Result<ByteView>(body);
}

Status decodePageImage(ByteView src, RecordType type, std::uint32_t pageSize, PageImageRecord& out) {
    const bool v2 = (type == RecordType::PageImageV2);
    if (!v2 && type != RecordType::PageImage) return Status::InvalidArgument("not a PageImage record");
    const std::size_t prefix = v2 ? kPageImageV2Prefix : kPageImageV1Prefix;
    if (src.size() < prefix) return Status::Corruption("PageImage record too short");
    out.pageId = readU64(src.data());
    out.type = static_cast<core::PageType>(readU32(src.data() + 8));
    out.payloadSize = readU32(src.data() + 12);
    out.itemCount = readU16(src.data() + 16);
    if (v2) {
        out.flags = readU16(src.data() + 18);
        out.overflowNext = readU64(src.data() + 20);
    } else {
        out.flags = 0;
        out.overflowNext = kInvalidPageId;
    }
    std::size_t payloadCapacity = pageSize - sizeof(core::PageHeader);
    if (src.size() < prefix + payloadCapacity) return Status::Corruption("PageImage payload truncated");
    out.payload.resize(payloadCapacity);
    std::memcpy(out.payload.data(), src.data() + prefix, payloadCapacity);
    return Status::OK();
}

Status decodeFreeListUpdate(ByteView src, FreeListUpdateRecord& out) {
    if (src.size() < 16) return Status::Corruption("FreeListUpdate record too short");
    out.retiringTxnId = readU64(src.data());
    std::uint64_t count = readU64(src.data() + 8);
    // Cap before any multiply/resize: corruption-controlled count must not
    // overflow 16 + count*8 or force an unbounded allocation (OOM/DoS).
    constexpr std::uint64_t kMaxRetiredPagesPerRecord = 1ull << 20;  // 1M page ids
    if (count > kMaxRetiredPagesPerRecord) {
        return Status::Corruption("FreeListUpdate page count exceeds maximum");
    }
    // Division form avoids uint64 overflow in count * 8.
    if (count > (src.size() - 16) / 8) {
        return Status::Corruption("FreeListUpdate page list truncated");
    }
    out.retiredPages.resize(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        out.retiredPages[static_cast<std::size_t>(i)] = readU64(src.data() + 16 + i * 8);
    }
    return Status::OK();
}

Status decodeCommit(ByteView src, CommitRecord& out) {
    if (src.size() < 36) return Status::Corruption("Commit record too short");
    out.txnId = readU64(src.data());
    out.masterRootPageId = readU64(src.data() + 8);
    out.freeListRootPageId = readU64(src.data() + 16);
    out.walSequence = readU64(src.data() + 24);
    out.pageSize = readU32(src.data() + 32);
    return Status::OK();
}

Status decodeCheckpoint(ByteView src, CheckpointRecord& out) {
    if (src.size() < 8) return Status::Corruption("Checkpoint record too short");
    out.upToTxnId = readU64(src.data());
    return Status::OK();
}

}  // namespace annpocketdb::wal
