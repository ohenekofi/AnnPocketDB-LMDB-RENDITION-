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

Status encodePageImage(const PageImageRecord& rec, std::uint32_t pageSize, ByteBuffer& out) {
    std::size_t payloadCapacity = pageSize - sizeof(core::PageHeader);
    ByteBuffer body;
    body.reserve(8 + 4 + 4 + 2 + payloadCapacity);
    body.resize(8 + 4 + 4 + 2);
    std::byte* p = body.data();
    writeU64(p, rec.pageId);
    writeU32(p + 8, static_cast<std::uint32_t>(rec.type));
    writeU32(p + 12, rec.payloadSize);
    writeU16(p + 16, rec.itemCount);
    body.insert(body.end(), rec.payload.begin(), rec.payload.end());
    return encodeGeneric(RecordType::PageImage, ByteView(body.data(), body.size()), out);
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

Status decodePageImage(ByteView src, std::uint32_t pageSize, PageImageRecord& out) {
    if (src.size() < 18) return Status::Corruption("PageImage record too short");
    out.pageId = readU64(src.data());
    out.type = static_cast<core::PageType>(readU32(src.data() + 8));
    out.payloadSize = readU32(src.data() + 12);
    out.itemCount = readU16(src.data() + 16);
    std::size_t payloadCapacity = pageSize - sizeof(core::PageHeader);
    if (src.size() < 18 + payloadCapacity) return Status::Corruption("PageImage payload truncated");
    out.payload.resize(payloadCapacity);
    std::memcpy(out.payload.data(), src.data() + 18, payloadCapacity);
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
