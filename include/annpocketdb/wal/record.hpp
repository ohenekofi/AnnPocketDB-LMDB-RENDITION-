#pragma once
//
// AnnPocketDb :: wal/record.hpp
//
// Write-ahead log record format. All records are self-describing,
// checksummed, and byte-order independent (little-endian on wire).
//
// Record layout on disk:
//   [uint32 recordCRC32C]
//   [uint32 recordLength]      // length of body only
//   [uint8  recordType]
//   [bytes  body]
//
// Types:
//   PageImage      – full page payload image (for redo). Legacy: carries no
//                    `flags` / `overflowNext`, so replaying it cut every
//                    multi-page overflow chain (blobs, large values) to its
//                    first page. Still decoded, never written.
//   PageImageV2    – PageImage plus the page header's `flags` and
//                    `overflowNext`. What logPageImage writes.
//   FreeListUpdate – serialized free-list page ids.
//   Commit         – transaction commit marker (txnId, rootPageId, etc).
//   Checkpoint     – indicates all prior records are durable on data pages.
//
#include <cstdint>
#include <vector>
#include "annpocketdb/core/page.hpp"
#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

namespace annpocketdb::wal {

// CRC(4) + length(4) + type(1). Keep in sync with encodeGeneric / validateRecordHeader.
constexpr std::size_t kRecordHeaderBytes = 9;

enum class RecordType : std::uint8_t {
    PageImage = 1,
    FreeListUpdate = 2,
    Commit = 3,
    Checkpoint = 4,
    PageImageV2 = 5,
};

struct PageImageRecord {
    PageId pageId = kInvalidPageId;
    core::PageType type = core::PageType::Leaf;
    std::uint32_t payloadSize = 0;
    std::uint16_t itemCount = 0;
    // V2 only; a legacy PageImage decodes to these defaults.
    std::uint16_t flags = 0;
    PageId overflowNext = kInvalidPageId;
    ByteBuffer payload;               // raw payload bytes (capacity = pageSize - sizeof(PageHeader))
};

struct FreeListUpdateRecord {
    std::vector<PageId> retiredPages;
    TxnId retiringTxnId = kInvalidTxnId;
};

struct CommitRecord {
    TxnId txnId = kInvalidTxnId;
    PageId masterRootPageId = kInvalidPageId;
    PageId freeListRootPageId = kInvalidPageId;
    SequenceNumber walSequence = 0;
    std::uint32_t pageSize = 0;
};

struct CheckpointRecord {
    TxnId upToTxnId = kInvalidTxnId;
};

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

// Encodes a record into an owned buffer (including CRC/length prefix).
// encodePageImage always writes RecordType::PageImageV2.
Status encodePageImage(const PageImageRecord& rec, std::uint32_t pageSize, ByteBuffer& out);
Status encodeFreeListUpdate(const FreeListUpdateRecord& rec, ByteBuffer& out);
Status encodeCommit(const CommitRecord& rec, ByteBuffer& out);
Status encodeCheckpoint(const CheckpointRecord& rec, ByteBuffer& out);

// Decodes a record from a buffer (after CRC/length prefix have been validated).
// `src` must point to the first byte after the 9-byte header (kRecordHeaderBytes).
// `type` selects the layout: RecordType::PageImage (legacy) or PageImageV2.
Status decodePageImage(ByteView src, RecordType type, std::uint32_t pageSize, PageImageRecord& out);
Status decodeFreeListUpdate(ByteView src, FreeListUpdateRecord& out);
Status decodeCommit(ByteView src, CommitRecord& out);
Status decodeCheckpoint(ByteView src, CheckpointRecord& out);

// Validates CRC and length header; returns the body view on success.
Result<ByteView> validateRecordHeader(ByteView data, RecordType& outType);

}  // namespace annpocketdb::wal
