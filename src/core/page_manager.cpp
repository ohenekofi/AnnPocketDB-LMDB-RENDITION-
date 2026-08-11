#include "annpocketdb/core/page_manager.hpp"

#include <cstring>
#include <random>
#include <algorithm>

#include "annpocketdb/btree/node_codec.hpp"
#include "annpocketdb/encryption/page_cipher.hpp"
#include "annpocketdb/util/crc32c.hpp"
#include "annpocketdb/wal/wal.hpp"
#if defined(ANNPOCKETDB_WITH_ENCRYPTION)
#include "sha256.hpp"
#endif

namespace annpocketdb::core {

namespace {
constexpr std::size_t kPageSizeFieldOffset =
    sizeof(PageHeader) + offsetof(MetaPageBody, pageSize);  // absolute byte offset within page 0

Status doFlush(platform::MappedFile& mf, std::uint64_t offset, std::uint64_t length, SyncMode mode) {
    if (mode == SyncMode::NoSync) return Status::OK();
    // Durable => sync=true (fsync/MS_SYNC); Flush => sync=false (MS_ASYNC style).
    return mf.flush(offset, length, mode == SyncMode::Durable);
}

// Apply env durability flags to a caller-requested commit SyncMode for data pages.
SyncMode effectiveDataMode(SyncMode requested, const DurabilityFlags& d) {
    if (d.noSync) return SyncMode::NoSync;
    if (d.mapAsync && requested == SyncMode::Durable) return SyncMode::Flush;
    return requested;
}

// Meta page sync: MDB_NOMETASYNC skips durable meta fsync.
SyncMode effectiveMetaMode(SyncMode requested, const DurabilityFlags& d) {
    if (d.noSync || d.noMetaSync) return SyncMode::NoSync;
    if (d.mapAsync && requested == SyncMode::Durable) return SyncMode::Flush;
    return requested;
}
}  // namespace

PageManager::~PageManager() {
    if (mappedFile_.isOpen()) (void)mappedFile_.close();
}

void PageManager::publishSnapshot(int metaIndex, TxnId txnId, PageId masterRoot,
                                  PageId freeListRoot, SequenceNumber walSeq) {
    // Enter critical section (odd sequence) so currentSnapshot() retries.
    snapshotSeq_.fetch_add(1, std::memory_order_release);
    activeMetaIndex_.store(metaIndex, std::memory_order_relaxed);
    masterRootPageId_.store(masterRoot, std::memory_order_relaxed);
    freeListRootPageId_.store(freeListRoot, std::memory_order_relaxed);
    walSequence_.store(walSeq, std::memory_order_relaxed);
    // Publish txnId last among the payload fields, then make the sequence even.
    lastCommittedTxnId_.store(txnId, std::memory_order_relaxed);
    snapshotSeq_.fetch_add(1, std::memory_order_release);
}

PageHeader* PageManager::pageAt(PageId id) {
    return reinterpret_cast<PageHeader*>(mappedFile_.data() + offsetOf(id));
}
const PageHeader* PageManager::pageAtConst(PageId id) const {
    return reinterpret_cast<const PageHeader*>(mappedFile_.data() + offsetOf(id));
}

std::uint32_t PageManager::computePageCrc(const PageHeader* header) const {
    PageHeader tmp = *header;
    tmp.crc32c = 0;
    std::uint32_t crc = util::crc32c(0, &tmp, sizeof(tmp));
    crc = util::crc32cExtend(crc, ByteView(payload(header), header->payloadSize));
    return crc;
}

// ---------------------------------------------------------------------------
// Encryption plumbing (see core/page_manager.hpp for the method contracts)
// ---------------------------------------------------------------------------

PageHeader* PageManager::allocateEncryptedScratch(PageId id) {
    auto buf = std::make_unique<std::byte[]>(pageSize_);
    PageHeader* header = reinterpret_cast<PageHeader*>(buf.get());
    std::lock_guard<std::mutex> lock(plainCacheMutex_);
    plainCache_[id] = std::move(buf);
    return header;
}

PageHeader* PageManager::currentMutablePage(PageId id) {
    if (!encryptionEnabled_) return pageAt(id);
    std::lock_guard<std::mutex> lock(plainCacheMutex_);
    auto it = plainCache_.find(id);
    // Must already exist: created by allocatePage/writePageAt for this id
    // earlier in the same call chain (this method's whole contract).
    return it != plainCache_.end() ? reinterpret_cast<PageHeader*>(it->second.get()) : pageAt(id);
}

#if defined(ANNPOCKETDB_WITH_ENCRYPTION)
void PageManager::encryptAndPersist(PageHeader* header) {
    PageId id = header->pageId;
    std::uint64_t oldCounter = 0;
    encryption::Mac oldMac{};
    encSidecar_.read(id, oldCounter, oldMac);
    std::uint64_t counter = oldCounter + 1;  // never reuse a (pageId, counter) nonce

    auto cipherBuf = std::make_unique<std::uint8_t[]>(pageSize_);
    encryption::Mac mac{};
    encryption::encryptPage(encryptionKey_, id, counter,
                            ByteView(reinterpret_cast<const std::byte*>(header), pageSize_),
                            cipherBuf.get(), mac);

    std::memcpy(pageAt(id), cipherBuf.get(), pageSize_);
    encSidecar_.write(id, counter, mac);
}

Result<const PageHeader*> PageManager::getEncryptedPage(PageId id) const {
    {
        std::lock_guard<std::mutex> lock(plainCacheMutex_);
        auto it = plainCache_.find(id);
        if (it != plainCache_.end()) {
            return Result<const PageHeader*>(reinterpret_cast<const PageHeader*>(it->second.get()));
        }
    }
    std::uint64_t offset = offsetOf(id);
    if (offset + pageSize_ > mappedFile_.fileSize()) {
        return Status::PageNotFound("page id " + std::to_string(id) + " is beyond end of file");
    }
    std::uint64_t counter = 0;
    encryption::Mac mac{};
    encSidecar_.read(id, counter, mac);
    if (counter == 0) {
        return Status::NotFound("page id " + std::to_string(id) + " has no encryption metadata");
    }

    auto buf = std::make_unique<std::byte[]>(pageSize_);
    const std::byte* cipherPtr = reinterpret_cast<const std::byte*>(pageAtConst(id));
    bool ok = encryption::decryptPage(encryptionKey_, id, counter,
                                      reinterpret_cast<const std::uint8_t*>(cipherPtr), pageSize_, mac,
                                      reinterpret_cast<std::uint8_t*>(buf.get()));
    if (!ok) {
        return Status::Corruption("AEAD decryption failed on page " + std::to_string(id) +
                                  " (corrupt ciphertext, wrong key, or tampered data)");
    }
    const PageHeader* header = reinterpret_cast<const PageHeader*>(buf.get());
    if (header->pageId != id) {
        return Status::Corruption("page id mismatch after decryption: expected " + std::to_string(id) +
                                  " found " + std::to_string(header->pageId));
    }
    if (header->payloadSize > payloadCapacity(pageSize_)) {
        return Status::Corruption("payloadSize exceeds page capacity on page " + std::to_string(id));
    }
    if (durableCrc_ && computePageCrc(header) != header->crc32c) {
        return Status::Corruption("CRC32C mismatch on decrypted page " + std::to_string(id));
    }
    if (sha256ChecksumEnabled_) {
        Status cs = verifyChecksum(header);
        if (!cs.ok()) return cs;
    }

    std::lock_guard<std::mutex> lock(plainCacheMutex_);
    auto& slot = plainCache_[id];
    slot = std::move(buf);
    return Result<const PageHeader*>(reinterpret_cast<const PageHeader*>(slot.get()));
}
#else
void PageManager::encryptAndPersist(PageHeader*) {
    // Unreachable: encryptionEnabled_ can only be true when this macro is
    // defined (enforced in open()).
}
Result<const PageHeader*> PageManager::getEncryptedPage(PageId) const {
    return Status::NotSupported("encryption support was not built");
}
#endif

#if defined(ANNPOCKETDB_WITH_ENCRYPTION)
void PageManager::writeChecksum(const PageHeader* header) {
    auto digest = sha256::hash(header, pageSize_);
    checksumSidecar_.write(header->pageId, digest);
}

Status PageManager::verifyChecksum(const PageHeader* header) const {
    std::array<std::uint8_t, ChecksumSidecar::kRecordSize> stored{};
    checksumSidecar_.read(header->pageId, stored);
    auto computed = sha256::hash(header, pageSize_);
    if (stored != computed) {
        return Status::Corruption("SHA-256 checksum mismatch on page " + std::to_string(header->pageId));
    }
    return Status::OK();
}
#else
void PageManager::writeChecksum(const PageHeader*) {
    // Unreachable: sha256ChecksumEnabled_ can only be true when this macro
    // is defined (enforced in open()).
}
Status PageManager::verifyChecksum(const PageHeader*) const { return Status::OK(); }
#endif

Result<std::unique_ptr<PageManager>> PageManager::open(const std::filesystem::path& path,
                                                         const PageManagerOptions& options) {
    if (options.pageSize < kMinPageSize || options.pageSize > kMaxPageSize) {
        return Status::InvalidArgument("pageSize out of supported range");
    }
#if !defined(ANNPOCKETDB_WITH_ENCRYPTION)
    if (options.encryptionEnabled) {
        return Status::NotSupported("encryption support was not built (ANNPOCKETDB_WITH_ENCRYPTION)");
    }
    if (options.sha256ChecksumEnabled) {
        return Status::NotSupported("SHA-256 checksum support was not built (ANNPOCKETDB_WITH_ENCRYPTION)");
    }
#endif
#if !defined(ANNPOCKETDB_WITH_COMPRESSION)
    if (options.compressionEnabled) {
        return Status::NotSupported("compression support was not built (ANNPOCKETDB_WITH_COMPRESSION)");
    }
#endif

    std::error_code ec;
    bool existedBefore = std::filesystem::exists(path, ec) && !ec && std::filesystem::file_size(path, ec) > 0;

    std::uint64_t initialSizeForOpen =
        existedBefore ? 0
                       : std::max<std::uint64_t>(
                             options.initialFileSize ? options.initialFileSize
                                                      : static_cast<std::uint64_t>(options.pageSize) * 16,
                             static_cast<std::uint64_t>(options.pageSize) * 2);

    auto mode = options.readOnly ? platform::OpenMode::ReadOnly : platform::OpenMode::ReadWrite;
    auto mfResult = platform::MappedFile::open(path, mode, initialSizeForOpen, options.maxDatabaseSize);
    if (!mfResult) return mfResult.status();

    auto pm = std::unique_ptr<PageManager>(new PageManager());
    pm->mappedFile_ = mfResult.moveValue();
    pm->durableCrc_ = options.durableCrc;
    pm->durability_ = options.durability;
    pm->pageSize_ = options.pageSize;
    pm->encryptionEnabled_ = options.encryptionEnabled;
    pm->encryptionKey_ = options.encryptionKey;
    pm->sha256ChecksumEnabled_ = options.sha256ChecksumEnabled;
    pm->compressionEnabled_ = options.compressionEnabled;
    pm->compressionMinSize_ = options.compressionMinSize;

    // Writer gate: .lock file when multiProcess, else in-process only.
    Status lockS = pm->writerGate_.open(path, options.readOnly, options.multiProcess, options.maxReaders);
    if (!lockS.ok()) return lockS;

    if (pm->encryptionEnabled_) {
        // Opened before loadExistingFile/initializeNewFile: loading an
        // existing encrypted file's free list calls getPage() on data
        // pages, which needs the sidecar already available to decrypt them.
        Status encS = pm->encSidecar_.open(path, options.readOnly, options.maxDatabaseSize, options.pageSize);
        if (!encS.ok()) return encS;
    }
    if (pm->sha256ChecksumEnabled_) {
        Status csS = pm->checksumSidecar_.open(path, options.readOnly, options.maxDatabaseSize, options.pageSize);
        if (!csS.ok()) return csS;
    }

    Status s = existedBefore ? pm->loadExistingFile(options) : pm->initializeNewFile(options);
    if (!s.ok()) return s;
    return Result<std::unique_ptr<PageManager>>(std::move(pm));
}

Status PageManager::initializeNewFile(const PageManagerOptions& options) {
    Status rs = mappedFile_.resize(std::max<std::uint64_t>(mappedFile_.fileSize(),
                                                             static_cast<std::uint64_t>(pageSize_) * 2));
    if (!rs.ok()) return rs;

    nextFreePageId_ = 2;
    publishSnapshot(/*metaIndex=*/0, /*txnId=*/1, kInvalidPageId, kInvalidPageId, /*walSeq=*/0);

    Status s0 = writeMetaSlot(0, lastCommittedTxnId_.load(std::memory_order_relaxed),
                               masterRootPageId_.load(std::memory_order_relaxed),
                               freeListRootPageId_.load(std::memory_order_relaxed),
                               walSequence_.load(std::memory_order_relaxed), SyncMode::Durable);
    if (!s0.ok()) return s0;
    Status s1 = writeMetaSlot(1, lastCommittedTxnId_.load(std::memory_order_relaxed),
                               masterRootPageId_.load(std::memory_order_relaxed),
                               freeListRootPageId_.load(std::memory_order_relaxed),
                               walSequence_.load(std::memory_order_relaxed), SyncMode::Durable);
    if (!s1.ok()) return s1;
    (void)options;
    return Status::OK();
}

MetaPageBody PageManager::readMetaBody(int slotIndex) const {
    MetaPageBody body{};
    const PageHeader* header = pageAtConst(static_cast<PageId>(slotIndex));
    std::memcpy(&body, payload(header), sizeof(body));
    return body;
}

bool PageManager::pickWinningMetaSlot(int& winner, MetaPageBody& outBody) const {
    bool slotValid[2] = {false, false};
    MetaPageBody slotBody[2];
    for (int slot = 0; slot < 2; ++slot) {
        const PageHeader* header = pageAtConst(static_cast<PageId>(slot));
        if (header->type != PageType::Meta) continue;
        if (header->payloadSize != sizeof(MetaPageBody)) continue;
        if (durableCrc_ && computePageCrc(header) != header->crc32c) continue;
        slotBody[slot] = readMetaBody(slot);
        if (slotBody[slot].magic != kMagic) continue;
        slotValid[slot] = true;
    }

    if (!slotValid[0] && !slotValid[1]) return false;

    if (slotValid[0] && slotValid[1]) {
        bool committed0 = slotBody[0].commitState == MetaCommitState::Committed;
        bool committed1 = slotBody[1].commitState == MetaCommitState::Committed;
        if (committed0 && committed1) {
            winner = (slotBody[1].lastCommittedTxnId > slotBody[0].lastCommittedTxnId) ? 1 : 0;
        } else if (committed0 != committed1) {
            // A Prepared/RolledBack slot must never outrank an actually
            // Committed one, regardless of txnId -- this is what makes
            // prepare() safe: a crash before the coordinator's decision must
            // not resurrect an in-doubt transaction as if it had committed.
            winner = committed0 ? 0 : 1;
        } else {
            // Neither slot is Committed -- should not happen in practice
            // (initializeNewFile starts both Committed, and prepareCommit
            // only ever touches the *inactive* slot, leaving the other
            // one's prior Committed state intact). Fall back to the higher
            // txnId as the least-bad choice among an already odd-looking pair.
            winner = (slotBody[1].lastCommittedTxnId > slotBody[0].lastCommittedTxnId) ? 1 : 0;
        }
    } else {
        winner = slotValid[0] ? 0 : 1;
    }

    outBody = slotBody[winner];
    return true;
}

Status PageManager::loadExistingFile(const PageManagerOptions& options) {
    if (mappedFile_.fileSize() < kPageSizeFieldOffset + sizeof(std::uint32_t)) {
        return Status::Corruption("file too small to contain a meta page");
    }

    std::uint32_t candidatePageSize = 0;
    std::memcpy(&candidatePageSize, mappedFile_.data() + kPageSizeFieldOffset, sizeof(candidatePageSize));
    if (candidatePageSize < kMinPageSize || candidatePageSize > kMaxPageSize) {
        return Status::InvalidFile("stored page size is out of range; file is not a valid AnnPocketDb database");
    }
    pageSize_ = candidatePageSize;

    if (mappedFile_.fileSize() < static_cast<std::uint64_t>(pageSize_) * 2) {
        return Status::Corruption("file too small to contain both meta pages");
    }

    int winner;
    MetaPageBody body;
    if (!pickWinningMetaSlot(winner, body)) {
        return Status::Corruption(
            "both meta pages failed validation; database requires WAL replay or backup restore");
    }

    if (body.formatVersion != kFormatVersion) {
        return Status::VersionMismatch("unsupported on-disk format version " + std::to_string(body.formatVersion));
    }
    if (options.pageSize != kDefaultPageSize && options.pageSize != body.pageSize) {
        return Status::Incompatible("requested pageSize does not match the database's stored pageSize");
    }

    nextFreePageId_ = body.nextFreePageId;
    publishSnapshot(winner, body.lastCommittedTxnId, body.masterRootPageId,
                    body.freeListRootPageId, body.walSequence);

    Status ls = freeList_.load(freeListRootPageId_.load(std::memory_order_relaxed),
                               [this](PageId pid, std::vector<PageId>& ids, PageId& next) -> Status {
        auto pageResult = getPage(pid);
        if (!pageResult) return pageResult.status();
        const PageHeader* header = pageResult.value();
        if (header->type != PageType::Free) return Status::Corruption("free-list chain page has wrong type");
        // Depends on getPage() having validated payloadSize <= capacity (and, under
        // ANNPOCKETDB_PARANOID, that payloadSize % sizeof(PageId) == 0). Do not
        // bypass getPage() here or this resize becomes an unbounded trust of
        // on-disk bytes.
        std::size_t count = header->payloadSize / sizeof(PageId);
        ids.resize(count);
        std::memcpy(ids.data(), payload(header), count * sizeof(PageId));
        next = header->overflowNext;
        return Status::OK();
    });
    if (!ls.ok()) return ls;

    return Status::OK();
}

Status PageManager::writeMetaSlot(int slotIndex, TxnId txnId, PageId masterRoot, PageId freeListRoot,
                                   SequenceNumber walSeq, SyncMode mode, MetaCommitState commitState) {
    PageHeader* header = pageAt(static_cast<PageId>(slotIndex));
    std::memset(header, 0, pageSize_);
    header->pageId = static_cast<PageId>(slotIndex);
    header->txnId = txnId;
    header->type = PageType::Meta;
    header->flags = 0;
    header->itemCount = 0;
    header->overflowNext = kInvalidPageId;

    MetaPageBody body{};
    body.magic = kMagic;
    body.formatVersion = kFormatVersion;
    body.pageSize = pageSize_;
    body.featureFlags = kFeatureCrc32c | (compressionEnabled_ ? static_cast<std::uint32_t>(kFeatureCompression) : 0u);
    body.lastCommittedTxnId = txnId;
    body.masterRootPageId = masterRoot;
    body.freeListRootPageId = freeListRoot;
    body.nextFreePageId = nextFreePageId_;
    body.walSequence = walSeq;
    body.commitState = commitState;

    std::memcpy(payload(header), &body, sizeof(body));
    header->payloadSize = sizeof(body);
    header->crc32c = 0;
    header->crc32c = computePageCrc(header);

    SyncMode metaMode = effectiveMetaMode(mode, durability_);
    return doFlush(mappedFile_, offsetOf(static_cast<PageId>(slotIndex)), pageSize_, metaMode);
}

Status validatePage(const PageHeader* header, std::uint32_t pageSize) {
    if (header == nullptr) return Status::Corruption("null page header");
    if (pageSize < sizeof(PageHeader)) {
        return Status::Corruption("pageSize smaller than PageHeader");
    }
    const std::uint32_t capacity =
        static_cast<std::uint32_t>(pageSize - sizeof(PageHeader));
    if (header->payloadSize > capacity) {
        return Status::Corruption(
            "payloadSize " + std::to_string(header->payloadSize) +
            " exceeds capacity " + std::to_string(capacity));
    }

    switch (header->type) {
        case PageType::Leaf:
        case PageType::Branch:
            return btree::validateNodePage(header, pageSize);

        case PageType::Free:
            // Free-list pages store a packed array of PageId.
            // Callers (FreeList::load via installRecoveredCommit / loadExistingFile)
            // compute count = payloadSize / sizeof(PageId). That division is only
            // safe once payloadSize has been bounds-checked here.
            if (header->payloadSize % sizeof(PageId) != 0) {
                return Status::Corruption(
                    "free-list payloadSize is not a multiple of sizeof(PageId)");
            }
            return Status::OK();

        case PageType::Overflow:
        case PageType::Meta:
            // payloadSize already checked against capacity above.
            return Status::OK();

        default:
            return Status::Corruption(
                "unknown page type " +
                std::to_string(static_cast<int>(header->type)));
    }
}

Result<const PageHeader*> PageManager::getPage(PageId id) const {
    if (encryptionEnabled_ && id >= 2) return getEncryptedPage(id);

    std::uint64_t offset = offsetOf(id);
    if (offset + pageSize_ > mappedFile_.fileSize()) {
        return Status::PageNotFound("page id " + std::to_string(id) + " is beyond end of file");
    }
    const PageHeader* header = pageAtConst(id);
    if (header->pageId != id) {
        return Status::Corruption("page id mismatch: expected " + std::to_string(id) + " found " +
                                   std::to_string(header->pageId));
    }
    // Bound payloadSize *before* CRC: computePageCrc reads payloadSize bytes
    // from the mmap. An untrusted/corrupted payloadSize would OOB-read (up to
    // ~4 GiB) and the CRC check itself would be the crash vector.
    if (header->payloadSize > payloadCapacity(pageSize_)) {
        return Status::Corruption("payloadSize exceeds page capacity on page " +
                                   std::to_string(id));
    }
    if (durableCrc_ && computePageCrc(header) != header->crc32c) {
        return Status::Corruption("CRC32C mismatch on page " + std::to_string(id));
    }
    if (sha256ChecksumEnabled_ && id >= 2) {
        Status cs = verifyChecksum(header);
        if (!cs.ok()) return cs;
    }
#if defined(ANNPOCKETDB_PARANOID)
    // Structural bounds-check (slots/cells). payloadSize already checked above.
    // Only runs when CRC is also enabled; keeps default builds at LMDB-parity cost.
    if (durableCrc_) {
        Status vs = validatePage(header, pageSize_);
        if (!vs.ok()) return vs;
    }
#endif
    return Result<const PageHeader*>(header);
}

Snapshot PageManager::currentSnapshot() const {
    // Seqlock: odd sequence means a writer is mid-publish. Retry until we
    // observe a stable even sequence around a coherent set of fields.
    Snapshot snap;
    for (;;) {
        const std::uint64_t s1 = snapshotSeq_.load(std::memory_order_acquire);
        if (s1 & 1ull) continue;  // writer in progress
        snap.txnId = lastCommittedTxnId_.load(std::memory_order_relaxed);
        snap.masterRootPageId = masterRootPageId_.load(std::memory_order_relaxed);
        snap.freeListRootPageId = freeListRootPageId_.load(std::memory_order_relaxed);
        snap.walSequence = walSequence_.load(std::memory_order_relaxed);
        const std::uint64_t s2 = snapshotSeq_.load(std::memory_order_acquire);
        if (s1 == s2) return snap;
    }
}

Snapshot PageManager::snapshotForNewRead() const {
    if (!writerGate_.multiProcess()) return currentSnapshot();
    int winner;
    MetaPageBody body;
    if (!pickWinningMetaSlot(winner, body)) return currentSnapshot();
    Snapshot cached = currentSnapshot();
    if (body.lastCommittedTxnId <= cached.txnId) return cached;
    Snapshot fromDisk;
    fromDisk.txnId = body.lastCommittedTxnId;
    fromDisk.masterRootPageId = body.masterRootPageId;
    fromDisk.freeListRootPageId = body.freeListRootPageId;
    fromDisk.walSequence = body.walSequence;
    return fromDisk;
}

Status PageManager::refreshFromDiskIfStale() {
    if (!writerGate_.multiProcess()) return Status::OK();
    int winner;
    MetaPageBody body;
    if (!pickWinningMetaSlot(winner, body)) return Status::OK();
    if (body.lastCommittedTxnId <= lastCommittedTxnId_.load(std::memory_order_acquire)) {
        return Status::OK();
    }

    // Another process committed since we last synced. Reload the free-list
    // pool from the new root -- same chain-walk pattern loadExistingFile and
    // installRecoveredCommit use.
    Status ls = freeList_.load(body.freeListRootPageId,
                               [this](PageId pid, std::vector<PageId>& ids, PageId& next) -> Status {
        auto pageResult = getPage(pid);
        if (!pageResult) return pageResult.status();
        const PageHeader* header = pageResult.value();
        if (header->type != PageType::Free) return Status::Corruption("free-list chain page has wrong type");
        std::size_t count = header->payloadSize / sizeof(PageId);
        ids.resize(count);
        std::memcpy(ids.data(), payload(header), count * sizeof(PageId));
        next = header->overflowNext;
        return Status::OK();
    });
    if (!ls.ok() && body.freeListRootPageId != kInvalidPageId) return ls;

    if (body.nextFreePageId > nextFreePageId_) nextFreePageId_ = body.nextFreePageId;

    publishSnapshot(winner, body.lastCommittedTxnId, body.masterRootPageId, body.freeListRootPageId,
                    body.walSequence);

    // Unlike installRecoveredCommit (safe to blindly reclaim(recoveredTxnId)
    // only because "no active readers during recovery" holds there), this
    // process may have live registered readers with older snapshots, so we
    // must use the real oldest-active-snapshot floor -- the same call
    // commit() itself makes.
    freeList_.reclaim(writerGate_.oldestActiveSnapshot(body.lastCommittedTxnId));
    return Status::OK();
}

Status PageManager::ensureFileCovers(PageId id) {
    std::uint64_t required = offsetOf(id) + pageSize_;
    if (required <= mappedFile_.fileSize()) return Status::OK();
    constexpr std::uint64_t kGrowthPages = 1024;
    std::uint64_t growthChunk = kGrowthPages * pageSize_;
    std::uint64_t rounded = ((required + growthChunk - 1) / growthChunk) * growthChunk;
    Status rs = mappedFile_.resize(rounded);
    if (!rs.ok()) {
        rs = mappedFile_.resize(required);
        if (!rs.ok()) return rs;
    }
    return Status::OK();
}

void PageManager::noteRecoveredPageId(PageId id) {
    if (id >= nextFreePageId_) {
        nextFreePageId_ = id + 1;
    }
}

Result<PageHeader*> PageManager::allocatePage(TxnId writerTxnId, PageType type) {
    PageId id = freeList_.popReusablePage();
    if (id == kInvalidPageId) {
        id = nextFreePageId_++;
        Status rs = ensureFileCovers(id);
        if (!rs.ok()) {
            --nextFreePageId_;  // roll back the watermark, allocation failed
            return rs;
        }
    }

    PageHeader* header = encryptionEnabled_ ? allocateEncryptedScratch(id) : pageAt(id);
    std::memset(header, 0, pageSize_);
    header->pageId = id;
    header->txnId = writerTxnId;
    header->type = type;
    header->flags = 0;
    header->itemCount = 0;
    header->payloadSize = 0;
    header->overflowNext = kInvalidPageId;
    return Result<PageHeader*>(header);
}

Result<PageHeader*> PageManager::writePageAt(PageId id, TxnId writerTxnId, PageType type) {
    if (id < 2) {
        return Status::InvalidArgument("writePageAt cannot target meta page slots 0/1");
    }
    Status rs = ensureFileCovers(id);
    if (!rs.ok()) return rs;
    noteRecoveredPageId(id);

    PageHeader* header = encryptionEnabled_ ? allocateEncryptedScratch(id) : pageAt(id);
    std::memset(header, 0, pageSize_);
    header->pageId = id;
    header->txnId = writerTxnId;
    header->type = type;
    header->flags = 0;
    header->itemCount = 0;
    header->payloadSize = 0;
    header->overflowNext = kInvalidPageId;
    return Result<PageHeader*>(header);
}

void PageManager::retirePage(PageId id, TxnId writerTxnId) {
    freeList_.retire(id, writerTxnId);
    retirementLog_.push_back(id);
    if (wal_ != nullptr) {
        pendingRetirements_.push_back(id);
    }
}

void PageManager::rollbackRetirementsTo(std::size_t mark, TxnId writerTxnId) {
    while (retirementLog_.size() > mark) {
        PageId id = retirementLog_.back();
        retirementLog_.pop_back();
        freeList_.unretire(id, writerTxnId);
        if (!pendingRetirements_.empty() && pendingRetirements_.back() == id) {
            pendingRetirements_.pop_back();
        }
    }
}

void PageManager::finalizePage(PageHeader* header, std::uint32_t payloadSize, std::uint16_t itemCount) {
    header->payloadSize = payloadSize;
    header->itemCount = itemCount;
    header->crc32c = 0;
    header->crc32c = computePageCrc(header);

    // WAL redo: log the finalized page image so a crash before the meta flip
    // can still restore it. Skipped during recoveryMode_ so redo does not
    // re-append images into the log being recovered.
    if (!recoveryMode_ && wal_ != nullptr && header->type != PageType::Meta) {
        ByteView payloadView(payload(header), payloadCapacity(pageSize_));
        Status s = wal_->logPageImage(header->pageId, header, payloadView);
        if (!s.ok()) {
            (void)s;
        }
    }

    // Encrypts the just-finalized plaintext buffer into the mmap + sidecar.
    // WAL redo (above) already logged plaintext regardless, and redo itself
    // calls writePageAt + finalizePage again to restore a page -- so this
    // re-derives and rewrites the sidecar entry as an ordinary side effect
    // of normal recovery, with no WAL format change needed.
    if (encryptionEnabled_ && header->type != PageType::Meta) {
        encryptAndPersist(header);
    }

    // Optional additive integrity check, independent of encryption: always
    // hashes the plaintext buffer (same one CRC32C above just covered),
    // regardless of whether encryptAndPersist then also encrypted it.
    if (sha256ChecksumEnabled_ && header->type != PageType::Meta) {
        writeChecksum(header);
    }
}

Result<PageId> PageManager::persistFreeListAndWal(TxnId newTxnId, PageId newMasterRootPageId,
                                                   SequenceNumber walSequence, SyncMode mode) {
    auto allocateForFreelist = [this, newTxnId]() -> PageId {
        auto r = allocatePage(newTxnId, PageType::Free);
        return r ? r.value()->pageId : kInvalidPageId;
    };
    auto writeFreelistPage = [this](PageId pid, const std::vector<PageId>& ids, PageId next) {
        // currentMutablePage, not pageAt: `pid` was just handed out by
        // allocateForFreelist above (allocatePage), which already routed it
        // through allocateEncryptedScratch when encryptionEnabled_ -- must
        // reuse that same cached plaintext buffer rather than touching the
        // mmap (ciphertext) directly.
        PageHeader* header = currentMutablePage(pid);
        header->overflowNext = next;
        std::memcpy(payload(header), ids.data(), ids.size() * sizeof(PageId));
        finalizePage(header, static_cast<std::uint32_t>(ids.size() * sizeof(PageId)),
                     static_cast<std::uint16_t>(ids.size()));
    };

    const PageId prevFreeListRoot = freeListRootPageId_.load(std::memory_order_relaxed);
    auto persistResult = freeList_.persist(prevFreeListRoot, writeFreelistPage, allocateForFreelist);
    PageId newFreeListRoot = persistResult ? persistResult.value() : prevFreeListRoot;

    // WAL group-commit boundary: free-list retirements + commit record must be
    // durable *before* the meta-page flip. On crash after WAL commit but before
    // meta flip, recovery replays page images and reconstructs the same root.
    if (wal_ != nullptr) {
        if (!wal_->healthy()) {
            pendingRetirements_.clear();
            return Status::IOError("WAL is unhealthy; refusing commit");
        }
        if (!pendingRetirements_.empty()) {
            Status s = wal_->logFreeListUpdate(pendingRetirements_, newTxnId);
            if (!s.ok()) {
                pendingRetirements_.clear();
                return s;
            }
            pendingRetirements_.clear();
        }
        Status s = wal_->commit(newTxnId, newMasterRootPageId, newFreeListRoot, walSequence, mode);
        if (!s.ok()) return s;
    } else {
        pendingRetirements_.clear();
    }
    return Result<PageId>(newFreeListRoot);
}

Status PageManager::commit(TxnId newTxnId, PageId newMasterRootPageId, SequenceNumber walSequence, SyncMode mode) {
    auto flr = persistFreeListAndWal(newTxnId, newMasterRootPageId, walSequence, mode);
    if (!flr) return flr.status();
    PageId newFreeListRoot = flr.value();

    int inactiveSlot = 1 - activeMetaIndex_.load(std::memory_order_relaxed);
    Status s = writeMetaSlot(inactiveSlot, newTxnId, newMasterRootPageId, newFreeListRoot, walSequence, mode);
    if (!s.ok()) return s;

    // Meta is durable; publish the in-memory snapshot for concurrent readers.
    publishSnapshot(inactiveSlot, newTxnId, newMasterRootPageId, newFreeListRoot, walSequence);

    retirementLog_.clear();
    freeList_.reclaim(writerGate_.oldestActiveSnapshot(newTxnId));

    if (testPostMetaFlipHook_) testPostMetaFlipHook_();

    // Checkpoint may delete WAL segments whose page images are the only
    // durable copy of COW pages still sitting in the OS page cache. Before
    // allowing that, force data pages to stable storage whenever the caller
    // asked for Durable (or Flush). Skipping this is the classic
    // "checkpoint ate the only copy of unflushed pages" WAL bug: meta already
    // points at the new roots, recovery sees nothing to redo, and a power
    // loss loses the pages.
    if (wal_ != nullptr) {
        SyncMode dataMode = effectiveDataMode(mode, durability_);
        if (dataMode != SyncMode::NoSync) {
            Status fs = flushAll(dataMode);
            if (!fs.ok()) return fs;
        }
        Status cs = wal_->checkpoint(newTxnId, mode);
        if (!cs.ok()) {
            // Non-fatal for correctness of *this* commit (meta is already
            // published); next recovery would simply redo more work if the
            // segments are still present. Surface the error so operators
            // notice disk issues.
            return cs;
        }
    }
    return Status::OK();
}

Result<PageManager::PreparedHandle> PageManager::prepareCommit(TxnId newTxnId, PageId newMasterRootPageId,
                                                               SequenceNumber walSequence, SyncMode mode) {
    auto flr = persistFreeListAndWal(newTxnId, newMasterRootPageId, walSequence, mode);
    if (!flr) return flr.status();
    PageId newFreeListRoot = flr.value();

    int inactiveSlot = 1 - activeMetaIndex_.load(std::memory_order_relaxed);
    Status s = writeMetaSlot(inactiveSlot, newTxnId, newMasterRootPageId, newFreeListRoot, walSequence, mode,
                             MetaCommitState::Prepared);
    if (!s.ok()) return s;
    // Deliberately no publishSnapshot/retirementLog clear/reclaim/checkpoint
    // here -- this transaction is not decided yet. Those all happen in
    // finalizePreparedCommit, or never, if invalidatePreparedSlot runs instead.
    return Result<PreparedHandle>(PreparedHandle{inactiveSlot});
}

Status PageManager::finalizePreparedCommit(const PreparedHandle& handle, TxnId txnId, PageId masterRootPageId,
                                           SequenceNumber walSequence, SyncMode mode) {
    MetaPageBody prepared = readMetaBody(handle.slot);
    // Second required durable write: flip this *same* slot from Prepared to
    // Committed in place. Once this returns success the result must survive
    // a crash even a microsecond later -- this write, not the earlier
    // prepareCommit write, is what makes that guarantee true.
    Status s = writeMetaSlot(handle.slot, txnId, masterRootPageId, prepared.freeListRootPageId, walSequence, mode,
                             MetaCommitState::Committed);
    if (!s.ok()) return s;

    publishSnapshot(handle.slot, txnId, masterRootPageId, prepared.freeListRootPageId, walSequence);

    retirementLog_.clear();
    freeList_.reclaim(writerGate_.oldestActiveSnapshot(txnId));

    if (testPostMetaFlipHook_) testPostMetaFlipHook_();

    if (wal_ != nullptr) {
        SyncMode dataMode = effectiveDataMode(mode, durability_);
        if (dataMode != SyncMode::NoSync) {
            Status fs = flushAll(dataMode);
            if (!fs.ok()) return fs;
        }
        Status cs = wal_->checkpoint(txnId, mode);
        if (!cs.ok()) return cs;
    }
    return Status::OK();
}

Status PageManager::invalidatePreparedSlot(const PreparedHandle& handle, SyncMode mode) {
    // The prepared slot is already durable and structurally valid (correct
    // magic/CRC) -- unlike a normal abort (which durably wrote nothing),
    // this needs an explicit durable overwrite so it can never win a future
    // slot-selection comparison, even though it was never published as the
    // active snapshot.
    MetaPageBody body = readMetaBody(handle.slot);
    return writeMetaSlot(handle.slot, body.lastCommittedTxnId, body.masterRootPageId, body.freeListRootPageId,
                         body.walSequence, mode, MetaCommitState::RolledBack);
}

int PageManager::findLingeringPreparedSlot() const {
    for (int slot = 0; slot < 2; ++slot) {
        const PageHeader* header = pageAtConst(static_cast<PageId>(slot));
        if (header->type != PageType::Meta) continue;
        if (header->payloadSize != sizeof(MetaPageBody)) continue;
        if (durableCrc_ && computePageCrc(header) != header->crc32c) continue;
        MetaPageBody body = readMetaBody(slot);
        if (body.magic != kMagic) continue;
        if (body.commitState == MetaCommitState::Prepared) return slot;
    }
    return -1;
}


Status PageManager::installRecoveredCommit(TxnId recoveredTxnId, PageId masterRoot,
                                           PageId freeListRoot, SequenceNumber walSequence,
                                           SyncMode mode) {
    if (recoveredTxnId == kInvalidTxnId) return Status::OK();
    if (recoveredTxnId <= lastCommittedTxnId_.load(std::memory_order_acquire)) {
        // Meta already at least as new as the WAL commit — nothing to do.
        return Status::OK();
    }

    // prepareCommit() logs a WAL Commit record identically to a real commit()
    // (see persistFreeListAndWal) -- the WAL alone cannot distinguish "durably
    // logged" from "actually decided". If this recovered txn is still sitting
    // in a Prepared meta slot, the coordinator's decision never arrived;
    // pickWinningMetaSlot() already correctly kept the other (genuinely
    // Committed) slot active during open(). Do not resurrect an in-doubt
    // transaction as committed just because the WAL durably logged its data;
    // the underlying pages were still replayed by Wal::recover()'s Pass 2
    // above (harmless either way), so rollbackPrepared() and the on-disk
    // Prepared slot remain fully correct.
    int preparedSlot = findLingeringPreparedSlot();
    if (preparedSlot >= 0 && readMetaBody(preparedSlot).lastCommittedTxnId == recoveredTxnId) {
        return Status::OK();
    }

    // Reload free-list chain from the recovered free-list root.
    Status ls = freeList_.load(freeListRoot, [this](PageId pid, std::vector<PageId>& ids, PageId& next) -> Status {
        auto pageResult = getPage(pid);
        if (!pageResult) return pageResult.status();
        const PageHeader* header = pageResult.value();
        if (header->type != PageType::Free) return Status::Corruption("recovered free-list page has wrong type");
        // Depends on getPage() having validated payloadSize <= capacity (and, under
        // ANNPOCKETDB_PARANOID, that payloadSize % sizeof(PageId) == 0). Do not
        // bypass getPage() here or this resize becomes an unbounded trust of
        // on-disk bytes.
        std::size_t count = header->payloadSize / sizeof(PageId);
        ids.resize(count);
        std::memcpy(ids.data(), payload(header), count * sizeof(PageId));
        next = header->overflowNext;
        return Status::OK();
    });
    if (!ls.ok() && freeListRoot != kInvalidPageId) return ls;

    int inactiveSlot = 1 - activeMetaIndex_.load(std::memory_order_relaxed);
    Status s = writeMetaSlot(inactiveSlot, recoveredTxnId, masterRoot, freeListRoot, walSequence, mode);
    if (!s.ok()) return s;

    publishSnapshot(inactiveSlot, recoveredTxnId, masterRoot, freeListRoot, walSequence);

    // No active readers during recovery; promote any retired pages immediately.
    freeList_.reclaim(recoveredTxnId);
    return Status::OK();
}

Status PageManager::flushAll(SyncMode mode) {
    Status s = doFlush(mappedFile_, 0, mappedFile_.fileSize(), mode);
    if (!s.ok()) return s;
    if (encryptionEnabled_) {
        // Same durability barrier as the main file: this is what
        // commit()/finalizePreparedCommit call before WAL checkpoint prunes
        // old segments, so the sidecar must be at least as durable as the
        // page data it describes by that point too.
        Status es = encSidecar_.flush(mode == SyncMode::Durable);
        if (!es.ok()) return es;
    }
    if (sha256ChecksumEnabled_) {
        Status cs = checksumSidecar_.flush(mode == SyncMode::Durable);
        if (!cs.ok()) return cs;
    }
    return Status::OK();
}

Status PageManager::setMapSize(std::uint64_t newMaxBytes) {
    if (newMaxBytes <= mappedFile_.mappedSize()) return Status::OK();
    if (activeReaderCount() > 0) {
        return Status::Busy(
            "setMapSize requires zero active readers (mapping may move)");
    }
    // Align up to page size.
    std::uint64_t aligned = ((newMaxBytes + pageSize_ - 1) / pageSize_) * pageSize_;
    return mappedFile_.expandMap(aligned);
}

Status PageManager::close() {
    wal_ = nullptr;
    if (encryptionEnabled_) {
        Status es = encSidecar_.close();
        if (!es.ok()) return es;
    }
    if (sha256ChecksumEnabled_) {
        Status cs = checksumSidecar_.close();
        if (!cs.ok()) return cs;
    }
    return mappedFile_.close();
}

}  // namespace annpocketdb::core
