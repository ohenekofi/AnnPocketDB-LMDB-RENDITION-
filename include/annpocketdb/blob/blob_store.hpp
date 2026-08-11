#pragma once
//
// AnnPocketDb :: blob/blob_store.hpp
//
// Large-object storage built on Overflow page chains.
//
// On-disk layout (first page of chain):
//   payload: [u32 magic 'BLOB'][u64 totalLen][data bytes...]
// Continuation pages:
//   payload: [data bytes...]
//   header.overflowNext links the chain; last page has kInvalidPageId.
//
// BlobId is the PageId of the first overflow page. totalLen is embedded in
// the head page so readers do not need an external length.
//
// Integration with the KV layer (optional value encoding):
//   putBlob/getBlob/removeBlob on Environment write a small BlobRef value
//   into the master keyspace: "BR1\0" + pageId(u64 LE) + totalLen(u64 LE).
//   Removing a key that holds a BlobRef also retires the overflow chain.
//
#include <cstdint>
#include <memory>
#include <vector>

#include "annpocketdb/core/page_manager.hpp"
#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

namespace annpocketdb::blob {

using BlobId = PageId;
constexpr BlobId kInvalidBlobId = kInvalidPageId;

constexpr std::uint32_t kBlobMagic = 0x424C4F42u;  // 'BLOB' LE
constexpr std::size_t kBlobHeadHeaderBytes = 4 + 8;  // magic + totalLen

// Wire format for a KV value that points at a blob chain.
inline constexpr std::string_view kBlobRefPrefix = "BR1";  // + NUL + ids

struct BlobRef {
    BlobId id = kInvalidBlobId;
    std::uint64_t totalLen = 0;
};

ByteBuffer encodeBlobRef(const BlobRef& ref);
Status decodeBlobRef(ByteView value, BlobRef& out);
bool isBlobRef(ByteView value);

// ---------------------------------------------------------------------------
// BlobWriter — streaming append under the writer txn
// ---------------------------------------------------------------------------

class BlobWriter {
public:
    BlobWriter(core::PageManager& pm, TxnId txnId);
    ~BlobWriter();

    BlobWriter(const BlobWriter&) = delete;
    BlobWriter& operator=(const BlobWriter&) = delete;
    BlobWriter(BlobWriter&&) noexcept;
    BlobWriter& operator=(BlobWriter&&) noexcept;

    Status write(ByteView data);
    // Flushes the last page, writes totalLen into the head header, returns id.
    Result<BlobId> finalize();

    std::uint64_t bytesWritten() const { return totalLen_; }
    bool finalized() const { return finalized_; }

private:
    Status ensurePage(/*isHead*/ bool head);
    Status sealCurrentPage(PageId next);

    core::PageManager* pm_ = nullptr;
    TxnId txnId_ = kInvalidTxnId;
    std::size_t pageCap_ = 0;

    std::vector<PageId> pageIds_;
    core::PageHeader* current_ = nullptr;  // writable current page
    core::PageHeader* head_ = nullptr;     // first page (patched on finalize)
    std::uint32_t currentUsed_ = 0;
    std::uint32_t currentCap_ = 0;
    std::uint32_t headUsed_ = 0;           // payload size of head when sealed
    std::uint64_t totalLen_ = 0;
    bool finalized_ = false;
};

// ---------------------------------------------------------------------------
// BlobReader — streaming read (snapshot-safe via getPage)
// ---------------------------------------------------------------------------

class BlobReader {
public:
    // Opens a blob; reads totalLen from the head page header.
    static Result<BlobReader> open(core::PageManager& pm, BlobId blobId);

    BlobReader(const BlobReader&) = delete;
    BlobReader& operator=(const BlobReader&) = delete;
    BlobReader(BlobReader&&) noexcept = default;
    BlobReader& operator=(BlobReader&&) noexcept = default;

    // Reads up to dst.size() bytes into dst; returns bytes read (0 = EOF).
    Result<std::uint64_t> read(std::byte* dst, std::size_t dstSize);
    Result<std::uint64_t> read(ByteBuffer& dst);

    Status seek(std::uint64_t offset);

    BlobId id() const { return headPageId_; }
    std::uint64_t totalLength() const { return totalLen_; }
    std::uint64_t currentOffset() const { return offset_; }

private:
    BlobReader() = default;
    Status loadHead();

    core::PageManager* pm_ = nullptr;
    BlobId headPageId_ = kInvalidBlobId;
    std::uint64_t totalLen_ = 0;
    std::uint64_t offset_ = 0;
    PageId currentPageId_ = kInvalidBlobId;
    std::uint32_t currentPageOffset_ = 0;  // offset within current page payload
};

// ---------------------------------------------------------------------------
// BlobStore
// ---------------------------------------------------------------------------

class BlobStore {
public:
    explicit BlobStore(core::PageManager& pm) : pm_(pm) {}

    Result<BlobRef> put(ByteView data, TxnId txnId);
    Result<ByteBuffer> get(BlobId blobId) const;
    // Retire every page in the chain (writer txn id for free-list stage).
    Status remove(BlobId blobId, TxnId txnId);

    Result<BlobWriter> openWriter(TxnId txnId);
    Result<BlobReader> openReader(BlobId blobId) const;

    std::size_t pagePayloadCapacity() const;

private:
    core::PageManager& pm_;
};

}  // namespace annpocketdb::blob
