#pragma once
//
// AnnPocketDb :: core/page_manager.hpp
//
// The page manager is the foundation of the storage layer: every other
// component (B+tree, transactions, indexes, blob storage) reads and writes
// pages exclusively through this class. It owns the memory-mapped data
// file, the two meta pages and the atomic commit protocol between them,
// page allocation/deallocation (including MVCC-safe reuse), and page
// validation (CRC32C) on every read.
//
// Concurrency model: single writer, multiple readers. Readers only ever
// call getPage() (safe to call concurrently from any number of threads).
// Only the one active WriteTransaction calls allocatePage / commit,
// serialized by writerMutex().
//
// WAL integration (optional):
//   When a wal::Wal is attached via attachWal(), finalizePage() appends a
//   PageImage record and commit() appends a Commit record and flushes the
//   log *before* flipping the meta page. Recovery uses writePageAt() to
//   restore page images at their logged page ids (allocatePage cannot be
//   used for that — it assigns ids from the free list / watermark).
//
// No "writemap" toggle (LMDB's MDB_WRITEMAP): unlike LMDB, which defaults to
// a safer mode and offers writemap=True as an opt-in, faster-but-riskier
// alternative, this engine has only the one mode. allocatePage/writePageAt
// (and, when encryption is off, ordinary getPage() reads) hand callers a
// pointer directly into the live mmap; there is no copy-on-read/write-then-
// flush safety net to fall back to. Effectively every AnnPocketDb
// environment already runs in what LMDB calls writemap=True, with no safer
// default available.
//
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "annpocketdb/core/checksum_sidecar.hpp"
#include "annpocketdb/core/encryption_sidecar.hpp"
#include "annpocketdb/core/free_list.hpp"
#include "annpocketdb/core/meta_page.hpp"
#include "annpocketdb/core/page.hpp"
#include "annpocketdb/core/reader_table.hpp"
#include "annpocketdb/platform/interprocess_lock.hpp"
#include "annpocketdb/core/writer_gate.hpp"
#include "annpocketdb/platform/mapped_file.hpp"
#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

// Forward declaration — avoid a hard include cycle with wal/wal.hpp.
namespace annpocketdb::wal { class Wal; }

namespace annpocketdb::core {

// Per-commit durability. Combined with Environment durability flags
// (noSync / noMetaSync / mapAsync) in PageManager::commit.
enum class SyncMode { Durable, Flush, NoSync };

// LMDB-style environment durability flags (can be OR'd conceptually).
struct DurabilityFlags {
    // MDB_NOSYNC: never fsync; fastest, risk of last txn(s) loss on crash.
    bool noSync = false;
    // MDB_NOMETASYNC: skip durable fsync of the meta page; data pages still
    // follow the commit SyncMode / mapAsync policy.
    bool noMetaSync = false;
    // MDB_MAPASYNC: prefer asynchronous flushes (Flush) even when the
    // caller asked for Durable, unless noSync is set.
    bool mapAsync = false;
};

struct PageManagerOptions {
    std::uint32_t pageSize = kDefaultPageSize;
    std::uint64_t maxDatabaseSize = 1ull << 40;  // 1 TiB virtual reservation ceiling
    std::uint64_t initialFileSize = 0;           // 0 => pageSize * 16
    bool readOnly = false;
    // When false, skip the .lock sidecar — single-process only (no cross-process safety).
    bool multiProcess = true;
    bool durableCrc = true;
    DurabilityFlags durability;
    std::uint32_t maxReaders = 256;
    std::uint32_t maxNamedDatabases = 0;

    // Page-level XChaCha20-Poly1305 encryption (see encryption/page_cipher.hpp
    // and core/encryption_sidecar.hpp for the mechanism). Covers data pages
    // only (Leaf/Branch/Free/Overflow) -- meta pages 0/1 always stay
    // cleartext so the engine can read pageSize/format before any key
    // material exists. Ignored (must stay false) unless built with
    // ANNPOCKETDB_WITH_ENCRYPTION; PageManager::open() rejects
    // encryptionEnabled=true otherwise.
    bool encryptionEnabled = false;
    std::array<std::uint8_t, 32> encryptionKey{};

    // Optional SHA-256 checksum bolt-on (see core/checksum_sidecar.hpp),
    // additive alongside the always-on CRC32C -- independent of
    // encryptionEnabled (usable with either, both, or neither). Same build
    // requirement as encryption: ignored (must stay false) unless built
    // with ANNPOCKETDB_WITH_ENCRYPTION.
    bool sha256ChecksumEnabled = false;

    // Value/overflow-level LZ4 compression (see compression/lz4_codec.hpp
    // and BTree::maybeCompress) -- gates only whether *new* plain, non-dup
    // values get compressed on write; reading a value some other session
    // already compressed is always attempted regardless of this flag, as
    // long as this binary was built with ANNPOCKETDB_WITH_COMPRESSION.
    // Ignored (must stay false) unless built with that macro;
    // PageManager::open() rejects compressionEnabled=true otherwise.
    bool compressionEnabled = false;
    // Values smaller than this are never compressed -- not worth LZ4's
    // per-call overhead for tiny inputs.
    std::uint32_t compressionMinSize = 256;
};

struct Snapshot {
    TxnId txnId = kInvalidTxnId;
    PageId masterRootPageId = kInvalidPageId;
    PageId freeListRootPageId = kInvalidPageId;
    SequenceNumber walSequence = 0;
};

class PageManager {
public:
    static Result<std::unique_ptr<PageManager>> open(const std::filesystem::path& path,
                                                       const PageManagerOptions& options);

    ~PageManager();
    PageManager(const PageManager&) = delete;
    PageManager& operator=(const PageManager&) = delete;

    // ---- Read path ----
    Result<const PageHeader*> getPage(PageId id) const;
    static const std::byte* payload(const PageHeader* header) {
        return reinterpret_cast<const std::byte*>(header) + sizeof(PageHeader);
    }
    static std::byte* payload(PageHeader* header) {
        return reinterpret_cast<std::byte*>(header) + sizeof(PageHeader);
    }
    Snapshot currentSnapshot() const;
    // Lock-free, allocation-free snapshot derivation for a brand-new
    // ReadTransaction: in multi-process mode, re-derives the winning meta
    // slot directly from the mmap (same algorithm as loadExistingFile) and
    // returns whichever of {that, our cached snapshot} is newer, WITHOUT
    // publishing anything -- safe to call from any number of concurrent
    // reader threads/processes with zero synchronization against a local
    // writer's publishSnapshot(). In single-process mode, identical to
    // currentSnapshot() (the cache is always current there already).
    Snapshot snapshotForNewRead() const;

    // ---- Write path (writer-exclusive) ----
    Result<PageHeader*> allocatePage(TxnId writerTxnId, PageType type);
    void retirePage(PageId id, TxnId writerTxnId);
    // Nested-txn support: size of the per-writer retirement log.
    std::size_t retirementLogSize() const { return retirementLog_.size(); }
    // Undo retires recorded after `mark` (FreeList + WAL pending list).
    void rollbackRetirementsTo(std::size_t mark, TxnId writerTxnId);
    void finalizePage(PageHeader* header, std::uint32_t payloadSize, std::uint16_t itemCount);
    Status commit(TxnId newTxnId, PageId newMasterRootPageId, SequenceNumber walSequence, SyncMode mode);

    // ---- Two-phase commit (LMDB-inspired prepare/commit/abort split) ----
    //
    // A prepared-but-undecided transaction must never be mistaken for a
    // committed one if the process crashes before the final decision --
    // MetaPageBody::commitState is the on-disk marker that makes that true
    // (see loadExistingFile's slot-selection rule: Committed always
    // outranks Prepared/RolledBack regardless of txnId).
    struct PreparedHandle {
        int slot = -1;
    };
    // Durably writes this transaction's data + a Prepared meta slot (same
    // durability guarantee as commit()) WITHOUT publishing it as the active
    // snapshot. The writer gate is not released by this call -- the caller
    // (WriteTransaction::prepare) keeps holding it until finalizePreparedCommit
    // or invalidatePreparedSlot runs.
    Result<PreparedHandle> prepareCommit(TxnId newTxnId, PageId newMasterRootPageId,
                                         SequenceNumber walSequence, SyncMode mode);
    // Durably flips the prepared slot's commitState to Committed in place
    // (a second required durable write -- once this returns success the
    // result must survive a crash even a microsecond later) and publishes
    // it as the active snapshot.
    Status finalizePreparedCommit(const PreparedHandle& handle, TxnId txnId, PageId masterRootPageId,
                                  SequenceNumber walSequence, SyncMode mode);
    // Durably marks the prepared slot RolledBack so it can never win a
    // future slot-selection comparison. The slot was never published, so
    // there is no in-memory snapshot to unpublish.
    Status invalidatePreparedSlot(const PreparedHandle& handle, SyncMode mode = SyncMode::Durable);
    // Housekeeping for Environment::rollbackPrepared: re-scans both meta
    // slots independent of which is currently active/winning and reports
    // one left in the Prepared state (e.g. by a process that prepared a
    // transaction and then crashed before deciding), or -1 if none. Not
    // required for correctness -- loadExistingFile's winner rule already
    // ignores a lingering Prepared slot automatically -- this just lets a
    // coordinator explicitly resolve and reclaim it.
    int findLingeringPreparedSlot() const;

    // Recovery / WAL replay: ensure page `id` exists in the file (growing if
    // needed), then return a writable header at that exact id. Unlike
    // allocatePage(), this does *not* pull from the free list or bump the
    // allocation watermark for a fresh id — it materializes a specific
    // logged page id so redo can restore images bit-for-bit.
    Result<PageHeader*> writePageAt(PageId id, TxnId writerTxnId, PageType type);

    // ---- WAL attachment ----
    // Non-owning. Lifetime must exceed any commit that uses it (typically
    // Environment owns both and attaches after open). Pass nullptr to detach.
    void attachWal(wal::Wal* wal) { wal_ = wal; }
    wal::Wal* wal() const { return wal_; }

    // ---- Reader lifecycle ----
    ReaderHandle registerReader(TxnId snapshotTxnId) {
        return writerGate_.registerReader(snapshotTxnId);
    }
    void unregisterReader(ReaderHandle handle) {
        writerGate_.unregisterReader(handle);
    }

    // ---- Coordination / introspection ----
    WriterGate& writerMutex() { return writerGate_; }
    const WriterGate& writerMutex() const { return writerGate_; }
    std::uint32_t pageSize() const { return pageSize_; }
    std::uint64_t pageCount() const { return mappedFile_.fileSize() / pageSize_; }
    FreeList& freeList() { return freeList_; }
    const FreeList& freeList() const { return freeList_; }
    platform::MappedFile& mappedFile() { return mappedFile_; }
    const platform::MappedFile& mappedFile() const { return mappedFile_; }
    std::size_t activeReaderCount() const { return writerGate_.activeReaderCount(); }
    PageId nextFreePageId() const { return nextFreePageId_; }
    // Advance the allocation watermark during recovery when a logged page id
    // sits at or beyond the current watermark.
    void noteRecoveredPageId(PageId id);

    // Install a recovered Commit into the meta page when the WAL has a
    // newer txn than the on-disk meta. Reloads the free-list from
    // freeListRoot, updates in-memory snapshot fields, and durable-syncs
    // the inactive meta slot. No-op (OK) when recoveredTxnId <= lastCommittedTxnId_.
    // Must be called with no WAL attached (or recoveryLogging suppressed).
    Status installRecoveredCommit(TxnId recoveredTxnId,
                                  PageId masterRoot,
                                  PageId freeListRoot,
                                  SequenceNumber walSequence,
                                  SyncMode mode = SyncMode::Durable);

    // Multi-process staleness fix: re-syncs this process's cached committed
    // state (root/txnId/free-list/watermark) from the actual on-disk meta
    // page when a *different* OS process has committed since this process
    // last checked. No-op in single-process mode. Must be called only while
    // holding the exclusive writer lock (see WriteTransaction's constructor)
    // -- unlike installRecoveredCommit, this does NOT assume "no active
    // readers" and does NOT re-write the meta slot (the other process
    // already durably wrote it; we only adopt it into our own cache).
    Status refreshFromDiskIfStale();

    // Suppress WAL side-effects in finalizePage during recovery redo.
    void setRecoveryMode(bool on) { recoveryMode_ = on; }
    bool recoveryMode() const { return recoveryMode_; }

    // Test-only instrumentation: if set, called immediately after
    // publishSnapshot in commit()/finalizePreparedCommit(), before the WAL
    // flush/checkpoint step. Exists solely so crash-consistency tests can
    // deterministically land on "meta durably flipped, checkpoint/data-fsync
    // never ran" -- the hazard class named in commit()'s own comment above
    // its flushAll/checkpoint call. No-op (nullptr) in all normal operation;
    // never set outside test code.
    void setTestPostMetaFlipHook(void (*hook)()) { testPostMetaFlipHook_ = hook; }

    TxnId lastCommittedTxnId() const {
        return lastCommittedTxnId_.load(std::memory_order_acquire);
    }

    Status flushAll(SyncMode mode);
    // Grow the virtual map ceiling (LMDB mdb_env_set_mapsize). Must not be
    // called while readers or a write transaction are active — may move the
    // mapping. newMax must be > current mappedSize and page-aligned preferred.
    Status setMapSize(std::uint64_t newMaxBytes);
    std::uint64_t mapSize() const { return mappedFile_.mappedSize(); }
    DurabilityFlags& durabilityFlags() { return durability_; }
    const DurabilityFlags& durabilityFlags() const { return durability_; }
    bool encryptionEnabled() const { return encryptionEnabled_; }
    bool sha256ChecksumEnabled() const { return sha256ChecksumEnabled_; }
    bool compressionEnabled() const { return compressionEnabled_; }
    std::uint32_t compressionMinSize() const { return compressionMinSize_; }
    Status close();

private:
    PageManager() = default;

    PageHeader* pageAt(PageId id);
    const PageHeader* pageAtConst(PageId id) const;
    std::size_t offsetOf(PageId id) const { return static_cast<std::size_t>(id) * pageSize_; }
    Status ensureFileCovers(PageId id);

    // ---- Encryption plumbing (see encryption/page_cipher.hpp,
    // core/encryption_sidecar.hpp) -- data pages only (id >= 2); meta pages
    // are never routed through any of these. ----
    //
    // Allocates a fresh heap buffer for `id`, registers it in plainCache_
    // (overwriting any prior entry), and returns it. Used by
    // allocatePage/writePageAt in place of a raw mmap pointer when
    // encryptionEnabled_, so the rest of the engine writes plaintext fields
    // into *some* PageHeader* without ever touching the mmap (which holds
    // ciphertext) until finalizePage's encryptAndPersist step.
    PageHeader* allocateEncryptedScratch(PageId id);
    // The header a caller should write fields into for an id already
    // created earlier in this call chain (e.g. persistFreeListAndWal's
    // writeFreelistPage re-touching a page allocatePage just handed out):
    // the cached plaintext buffer when encryptionEnabled_, otherwise the
    // raw mmap pointer (identical to today's behavior).
    PageHeader* currentMutablePage(PageId id);
    // Encrypts the finalized plaintext buffer at `header` and writes
    // ciphertext into the mmap + a fresh nonce-counter/MAC into encSidecar_.
    // Called from finalizePage when encryptionEnabled_.
    void encryptAndPersist(PageHeader* header);
    // getPage()'s encrypted-data-page path: cache hit, or decrypt from mmap
    // ciphertext + encSidecar_ into a fresh cache entry, validating the AEAD
    // tag and (same as the plaintext path) pageId/payloadSize/CRC.
    Result<const PageHeader*> getEncryptedPage(PageId id) const;

    // ---- Optional SHA-256 checksum bolt-on (see core/checksum_sidecar.hpp)
    // -- independent of encryption, additive alongside CRC32C. ----
    // Computes the digest over `header`'s full pageSize_ bytes and writes it
    // to checksumSidecar_. Called from finalizePage when
    // sha256ChecksumEnabled_, on the plaintext buffer (whichever backing
    // store it lives in -- mmap directly, or an encrypted-mode heap buffer).
    void writeChecksum(const PageHeader* header);
    // Recomputes the digest over `header` and compares against
    // checksumSidecar_'s stored value for its pageId. Called from getPage
    // (plaintext path) and getEncryptedPage (post-decryption) when
    // sha256ChecksumEnabled_.
    Status verifyChecksum(const PageHeader* header) const;

    Status initializeNewFile(const PageManagerOptions& options);
    Status loadExistingFile(const PageManagerOptions& options);
    // Reads both meta slots directly from the mmap and picks the winner via
    // the durable commit protocol's tie-break rule (higher lastCommittedTxnId
    // wins when both are Committed; a Committed slot always outranks
    // Prepared/RolledBack regardless of txnId). Lock-free, const, safe to
    // call from any thread/process at any time -- the two-meta-slot commit
    // protocol guarantees every slot is either fully-old or fully-new, never
    // torn. Returns false if neither slot validates. Shared by
    // loadExistingFile, refreshFromDiskIfStale, and snapshotForNewRead.
    bool pickWinningMetaSlot(int& winner, MetaPageBody& outBody) const;
    MetaPageBody readMetaBody(int slotIndex) const;
    Status writeMetaSlot(int slotIndex, TxnId txnId, PageId masterRoot, PageId freeListRoot,
                          SequenceNumber walSeq, SyncMode mode,
                          MetaCommitState commitState = MetaCommitState::Committed);
    std::uint32_t computePageCrc(const PageHeader* header) const;

    // Shared by commit()/prepareCommit(): persists the free list and (if a
    // WAL is attached) logs the pending retirements + commit record. Returns
    // the new free-list root to write into the meta slot.
    Result<PageId> persistFreeListAndWal(TxnId newTxnId, PageId newMasterRootPageId,
                                         SequenceNumber walSequence, SyncMode mode);

    // Publish snapshot fields under a seqlock so concurrent readers in
    // currentSnapshot() never observe a torn (txnId, root) pair. Odd
    // snapshotSeq_ means a writer is mid-update; even means stable.
    void publishSnapshot(int metaIndex, TxnId txnId, PageId masterRoot,
                         PageId freeListRoot, SequenceNumber walSeq);

    platform::MappedFile mappedFile_;
    std::uint32_t pageSize_ = kDefaultPageSize;
    bool durableCrc_ = true;
    DurabilityFlags durability_;

    std::atomic<int> activeMetaIndex_{0};
    // Seqlock + snapshot fields (readers load via currentSnapshot / lastCommittedTxnId).
    std::atomic<std::uint64_t> snapshotSeq_{0};  // odd = writer publishing
    std::atomic<TxnId> lastCommittedTxnId_{0};
    std::atomic<PageId> masterRootPageId_{kInvalidPageId};
    std::atomic<PageId> freeListRootPageId_{kInvalidPageId};
    std::atomic<SequenceNumber> walSequence_{0};
    PageId nextFreePageId_ = 2;  // watermark; pages 0/1 are reserved for meta (writer-only)

    FreeList freeList_;
    // Writer gate: InterprocessLock (.lock) or in-process mutex+ReaderTable.
    WriterGate writerGate_;

    // Optional WAL (non-owning). When non-null, finalizePage logs page images
    // and commit() logs a commit record before the meta flip.
    wal::Wal* wal_ = nullptr;

    // Pages retired during the current write transaction; flushed to the WAL
    // as a FreeListUpdate record at commit time when a WAL is attached.
    std::vector<PageId> pendingRetirements_;
    // Ordered log of every retirePage in the current writer txn (for nested abort).
    std::vector<PageId> retirementLog_;

    // When true, finalizePage does not append to the WAL (recovery redo).
    bool recoveryMode_ = false;

    // See setTestPostMetaFlipHook() above.
    void (*testPostMetaFlipHook_)() = nullptr;

    // ---- Encryption state (see the plumbing methods above). Present
    // unconditionally (cheap, dependency-free types) even when built
    // without ANNPOCKETDB_WITH_ENCRYPTION; encryptionEnabled_ can only ever
    // become true when that macro is defined (enforced in open()), so the
    // rest of this state is simply unused dead weight otherwise. ----
    bool encryptionEnabled_ = false;
    std::array<std::uint8_t, 32> encryptionKey_{};
    EncryptionSidecar encSidecar_;
    // mutable: getPage()/getEncryptedPage() are const but must populate this
    // cache lazily on first decrypt, matching this codebase's existing
    // lazy-cache-under-const convention (see e.g. txn::ReadTransaction::subDbs_).
    mutable std::mutex plainCacheMutex_;
    mutable std::unordered_map<PageId, std::unique_ptr<std::byte[]>> plainCache_;

    // Present unconditionally for the same reason as the encryption state
    // above; sha256ChecksumEnabled_ can only become true when
    // ANNPOCKETDB_WITH_ENCRYPTION is defined (enforced in open()).
    bool sha256ChecksumEnabled_ = false;
    ChecksumSidecar checksumSidecar_;

    // ---- Compression state. Present unconditionally for the same reason as
    // the encryption/checksum state above; compressionEnabled_ can only
    // become true when ANNPOCKETDB_WITH_COMPRESSION is defined (enforced in
    // open()). Consumed by BTree via the accessors above -- PageManager
    // itself never compresses/decompresses anything (unlike encryption,
    // this isn't a page-level transform). ----
    bool compressionEnabled_ = false;
    std::uint32_t compressionMinSize_ = 256;
};

}  // namespace annpocketdb::core
