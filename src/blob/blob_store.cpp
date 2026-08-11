#include "annpocketdb/blob/blob_store.hpp"
#include "annpocketdb/util/bounded_walk.hpp"

#include <algorithm>
#include <cstring>

namespace annpocketdb::blob {

// ---------------------------------------------------------------------------
// BlobRef wire format: "BR1" \0 u64 id LE  u64 totalLen LE
// ---------------------------------------------------------------------------

ByteBuffer encodeBlobRef(const BlobRef& ref) {
    ByteBuffer out;
    out.resize(kBlobRefPrefix.size() + 1 + 8 + 8);
    std::memcpy(out.data(), kBlobRefPrefix.data(), kBlobRefPrefix.size());
    out[kBlobRefPrefix.size()] = std::byte{0};
    std::uint64_t id = ref.id;
    std::uint64_t len = ref.totalLen;
    std::memcpy(out.data() + kBlobRefPrefix.size() + 1, &id, 8);
    std::memcpy(out.data() + kBlobRefPrefix.size() + 1 + 8, &len, 8);
    return out;
}

bool isBlobRef(ByteView value) {
    if (value.size() != kBlobRefPrefix.size() + 1 + 8 + 8) return false;
    if (std::memcmp(value.data(), kBlobRefPrefix.data(), kBlobRefPrefix.size()) != 0) return false;
    return value[kBlobRefPrefix.size()] == std::byte{0};
}

Status decodeBlobRef(ByteView value, BlobRef& out) {
    if (!isBlobRef(value)) return Status::InvalidArgument("not a blob ref value");
    std::uint64_t id = 0, len = 0;
    std::memcpy(&id, value.data() + kBlobRefPrefix.size() + 1, 8);
    std::memcpy(&len, value.data() + kBlobRefPrefix.size() + 1 + 8, 8);
    out.id = id;
    out.totalLen = len;
    return Status::OK();
}

// ---------------------------------------------------------------------------
// BlobWriter
// ---------------------------------------------------------------------------

BlobWriter::BlobWriter(core::PageManager& pm, TxnId txnId)
    : pm_(&pm), txnId_(txnId), pageCap_(core::payloadCapacity(pm.pageSize())) {}

BlobWriter::~BlobWriter() {
    if (!finalized_ && !pageIds_.empty() && pm_ != nullptr) {
        for (PageId pid : pageIds_) pm_->retirePage(pid, txnId_);
    }
}

BlobWriter::BlobWriter(BlobWriter&& o) noexcept
    : pm_(o.pm_), txnId_(o.txnId_), pageCap_(o.pageCap_), pageIds_(std::move(o.pageIds_)),
      current_(o.current_), head_(o.head_), currentUsed_(o.currentUsed_), currentCap_(o.currentCap_),
      headUsed_(o.headUsed_), totalLen_(o.totalLen_), finalized_(o.finalized_) {
    o.pm_ = nullptr;
    o.current_ = nullptr;
    o.head_ = nullptr;
    o.finalized_ = true;
}

BlobWriter& BlobWriter::operator=(BlobWriter&& o) noexcept {
    if (this == &o) return *this;
    if (!finalized_ && !pageIds_.empty() && pm_ != nullptr) {
        for (PageId pid : pageIds_) pm_->retirePage(pid, txnId_);
    }
    pm_ = o.pm_;
    txnId_ = o.txnId_;
    pageCap_ = o.pageCap_;
    pageIds_ = std::move(o.pageIds_);
    current_ = o.current_;
    head_ = o.head_;
    currentUsed_ = o.currentUsed_;
    currentCap_ = o.currentCap_;
    headUsed_ = o.headUsed_;
    totalLen_ = o.totalLen_;
    finalized_ = o.finalized_;
    o.pm_ = nullptr;
    o.current_ = nullptr;
    o.head_ = nullptr;
    o.finalized_ = true;
    return *this;
}

Status BlobWriter::sealCurrentPage(PageId next) {
    if (current_ == nullptr) return Status::OK();
    current_->overflowNext = next;
    if (current_ == head_) headUsed_ = currentUsed_;
    pm_->finalizePage(current_, currentUsed_, 0);
    current_ = nullptr;
    currentUsed_ = 0;
    currentCap_ = 0;
    return Status::OK();
}

Status BlobWriter::ensurePage(bool head) {
    if (current_ != nullptr && currentUsed_ < currentCap_) return Status::OK();

    // Seal previous page linking to a page we are about to allocate — we need
    // the new id first.
    auto alloc = pm_->allocatePage(txnId_, core::PageType::Overflow);
    if (!alloc) return alloc.status();
    core::PageHeader* header = alloc.value();
    header->overflowNext = kInvalidPageId;

    if (current_ != nullptr) {
        current_->overflowNext = header->pageId;
        if (current_ == head_) headUsed_ = currentUsed_;
        pm_->finalizePage(current_, currentUsed_, 0);
    }

    current_ = header;
    pageIds_.push_back(header->pageId);
    if (head) head_ = header;

    if (head) {
        if (pageCap_ < kBlobHeadHeaderBytes) {
            return Status::Internal("page too small for blob header");
        }
        auto* p = core::PageManager::payload(current_);
        std::uint32_t magic = kBlobMagic;
        std::uint64_t zero = 0;
        std::memcpy(p, &magic, 4);
        std::memcpy(p + 4, &zero, 8);
        currentUsed_ = static_cast<std::uint32_t>(kBlobHeadHeaderBytes);
        currentCap_ = static_cast<std::uint32_t>(pageCap_);
    } else {
        currentUsed_ = 0;
        currentCap_ = static_cast<std::uint32_t>(pageCap_);
    }
    return Status::OK();
}

Status BlobWriter::write(ByteView data) {
    if (finalized_) return Status::Internal("blob writer already finalized");
    std::size_t off = 0;
    while (off < data.size()) {
        bool needHead = pageIds_.empty();
        Status s = ensurePage(needHead);
        if (!s.ok()) return s;
        std::uint32_t space = currentCap_ - currentUsed_;
        if (space == 0) {
            // Should not happen — ensurePage allocates when full.
            s = ensurePage(false);
            if (!s.ok()) return s;
            space = currentCap_ - currentUsed_;
        }
        std::uint32_t take = static_cast<std::uint32_t>(
            std::min<std::size_t>(space, data.size() - off));
        std::memcpy(core::PageManager::payload(current_) + currentUsed_,
                    data.data() + off, take);
        currentUsed_ += take;
        off += take;
        totalLen_ += take;
    }
    return Status::OK();
}

Result<BlobId> BlobWriter::finalize() {
    if (finalized_) return Status::Internal("blob writer already finalized");

    if (pageIds_.empty()) {
        Status s = ensurePage(true);
        if (!s.ok()) return s;
    }

    if (current_ != nullptr) {
        Status s = sealCurrentPage(kInvalidPageId);
        if (!s.ok()) return s;
    }

    if (head_ == nullptr) return Status::Internal("blob head missing");
    std::memcpy(core::PageManager::payload(head_) + 4, &totalLen_, 8);
    std::uint32_t headSize = headUsed_ != 0 ? headUsed_ : head_->payloadSize;
    pm_->finalizePage(head_, headSize, 0);

    finalized_ = true;
    return Result<BlobId>(pageIds_.front());
}

// ---------------------------------------------------------------------------
// BlobReader
// ---------------------------------------------------------------------------

Result<BlobReader> BlobReader::open(core::PageManager& pm, BlobId blobId) {
    if (blobId == kInvalidBlobId) return Status::InvalidArgument("invalid blob id");
    BlobReader r;
    r.pm_ = &pm;
    r.headPageId_ = blobId;
    r.currentPageId_ = blobId;
    Status s = r.loadHead();
    if (!s.ok()) return s;
    return Result<BlobReader>(std::move(r));
}

Status BlobReader::loadHead() {
    auto pageResult = pm_->getPage(headPageId_);
    if (!pageResult) return pageResult.status();
    const core::PageHeader* h = pageResult.value();
    if (h->type != core::PageType::Overflow) {
        return Status::Corruption("blob head is not an overflow page");
    }
    if (h->payloadSize < kBlobHeadHeaderBytes) {
        return Status::Corruption("blob head too small");
    }
    const std::byte* p = core::PageManager::payload(h);
    std::uint32_t magic = 0;
    std::memcpy(&magic, p, 4);
    if (magic != kBlobMagic) return Status::Corruption("blob magic mismatch");
    std::uint64_t claimed = 0;
    std::memcpy(&claimed, p + 4, 8);
    // A blob cannot exceed the mapped database reservation. Reject huge
    // on-disk totalLen before any allocation (BlobStore::get materializes
    // ByteBuffer(totalLength())).
    const std::uint64_t maxBytes = pm_->mappedFile().mappedSize();
    if (claimed > maxBytes) {
        return Status::Corruption(
            "blob totalLen " + std::to_string(claimed) +
            " exceeds database map size " + std::to_string(maxBytes));
    }
    totalLen_ = claimed;
    currentPageOffset_ = static_cast<std::uint32_t>(kBlobHeadHeaderBytes);
    offset_ = 0;
    return Status::OK();
}

Result<std::uint64_t> BlobReader::read(std::byte* dst, std::size_t dstSize) {
    if (offset_ >= totalLen_) return Result<std::uint64_t>(0);
    std::uint64_t toRead = std::min<std::uint64_t>(dstSize, totalLen_ - offset_);
    std::uint64_t readSoFar = 0;

    // Bound by page count so a zero-payload cycle on overflowNext cannot spin.
    const std::uint64_t chainCap =
        std::max<std::uint64_t>(pm_->pageCount(), 1) + 2;
    BoundedWalk walk(chainCap);
    while (readSoFar < toRead && currentPageId_ != kInvalidPageId) {
        Status bs = walk.step("blob read chain");
        if (!bs.ok()) return bs;
        auto pageResult = pm_->getPage(currentPageId_);
        if (!pageResult) return pageResult.status();
        const core::PageHeader* header = pageResult.value();
        if (header->type != core::PageType::Overflow) {
            return Status::Corruption("blob chain page has wrong type");
        }
        std::uint32_t pagePayload = header->payloadSize;
        if (currentPageOffset_ >= pagePayload) {
            // Empty/already-consumed page: advance, but the BoundedWalk above
            // guarantees we cannot loop forever on a self-cycle.
            currentPageOffset_ = 0;
            currentPageId_ = header->overflowNext;
            continue;
        }
        std::uint32_t avail = pagePayload - currentPageOffset_;
        std::uint64_t need = toRead - readSoFar;
        std::uint32_t take = static_cast<std::uint32_t>(std::min<std::uint64_t>(avail, need));
        std::memcpy(dst + readSoFar,
                    core::PageManager::payload(header) + currentPageOffset_, take);
        currentPageOffset_ += take;
        readSoFar += take;
        offset_ += take;
        if (currentPageOffset_ >= pagePayload) {
            currentPageOffset_ = 0;
            currentPageId_ = header->overflowNext;
        }
    }
    return Result<std::uint64_t>(readSoFar);
}

Result<std::uint64_t> BlobReader::read(ByteBuffer& dst) {
    if (dst.empty()) return Result<std::uint64_t>(0);
    return read(dst.data(), dst.size());
}

Status BlobReader::seek(std::uint64_t offset) {
    if (offset > totalLen_) return Status::InvalidArgument("blob seek past end");
    // Reset to head and skip.
    Status s = loadHead();
    if (!s.ok()) return s;
    currentPageId_ = headPageId_;
    offset_ = 0;
    // Skip `offset` data bytes (head starts after header).
    std::uint64_t skip = offset;
    const std::uint64_t chainCap =
        std::max<std::uint64_t>(pm_->pageCount(), 1) + 2;
    BoundedWalk walk(chainCap);
    while (skip > 0 && currentPageId_ != kInvalidPageId) {
        Status bs = walk.step("blob seek chain");
        if (!bs.ok()) return bs;
        auto pageResult = pm_->getPage(currentPageId_);
        if (!pageResult) return pageResult.status();
        const core::PageHeader* header = pageResult.value();
        std::uint32_t pagePayload = header->payloadSize;
        std::uint32_t dataStart = (currentPageId_ == headPageId_)
                                      ? static_cast<std::uint32_t>(kBlobHeadHeaderBytes)
                                      : 0;
        if (currentPageOffset_ < dataStart) currentPageOffset_ = dataStart;
        if (currentPageOffset_ >= pagePayload) {
            currentPageId_ = header->overflowNext;
            currentPageOffset_ = 0;
            continue;
        }
        std::uint32_t avail = pagePayload - currentPageOffset_;
        if (skip < avail) {
            currentPageOffset_ += static_cast<std::uint32_t>(skip);
            offset_ = offset;
            return Status::OK();
        }
        skip -= avail;
        currentPageId_ = header->overflowNext;
        currentPageOffset_ = 0;
    }
    offset_ = offset;
    return Status::OK();
}

// ---------------------------------------------------------------------------
// BlobStore
// ---------------------------------------------------------------------------

Result<BlobRef> BlobStore::put(ByteView data, TxnId txnId) {
    BlobWriter w(pm_, txnId);
    Status s = w.write(data);
    if (!s.ok()) return s;
    auto idR = w.finalize();
    if (!idR) return idR.status();
    BlobRef ref;
    ref.id = idR.value();
    ref.totalLen = data.size();
    return Result<BlobRef>(ref);
}

Result<ByteBuffer> BlobStore::get(BlobId blobId) const {
    auto readerR = BlobReader::open(pm_, blobId);
    if (!readerR) return readerR.status();
    BlobReader reader = readerR.moveValue();
    // totalLength() is already clamped in loadHead against mappedSize();
    // re-check before allocating so a future API change cannot reintroduce
    // the OOM path.
    const std::uint64_t len = reader.totalLength();
    if (len > pm_.mappedFile().mappedSize()) {
        return Status::Corruption("blob totalLen exceeds database map size");
    }
    ByteBuffer out(static_cast<std::size_t>(len));
    auto n = reader.read(out);
    if (!n) return n.status();
    if (n.value() != len) {
        return Status::Corruption("blob short read");
    }
    return Result<ByteBuffer>(std::move(out));
}

Status BlobStore::remove(BlobId blobId, TxnId txnId) {
    PageId cur = blobId;
    const std::uint64_t chainCap =
        std::max<std::uint64_t>(pm_.pageCount(), 1) + 2;
    BoundedWalk walk(chainCap);
    while (cur != kInvalidPageId) {
        Status bs = walk.step("blob remove chain");
        if (!bs.ok()) return bs;
        auto pageResult = pm_.getPage(cur);
        if (!pageResult) return pageResult.status();
        PageId next = pageResult.value()->overflowNext;
        pm_.retirePage(cur, txnId);
        cur = next;
    }
    return Status::OK();
}

Result<BlobWriter> BlobStore::openWriter(TxnId txnId) {
    return Result<BlobWriter>(BlobWriter(pm_, txnId));
}

Result<BlobReader> BlobStore::openReader(BlobId blobId) const {
    return BlobReader::open(pm_, blobId);
}

std::size_t BlobStore::pagePayloadCapacity() const {
    return core::payloadCapacity(pm_.pageSize());
}

}  // namespace annpocketdb::blob
