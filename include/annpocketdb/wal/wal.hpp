#pragma once
//
// AnnPocketDb :: wal/wal.hpp
//
// Multi-segment write-ahead log with streaming recovery.
//
// Layout (base path from EnvironmentOptions::walPath or data+".wal"):
//   {base}/                     directory
//     00000001.seg              segment files (fixed-width decimal names)
//     00000002.seg
//     ...
//
// Durability protocol:
//   1. finalizePage → PageImage records (buffered, then appended to the
//      current segment)
//   2. commit() → FreeListUpdate + Commit, flush + fsync of the active
//      segment — txn is recoverable
//   3. PageManager meta flip
//   4. checkpoint(upToTxnId) → write Checkpoint, fsync, then *delete*
//      segments whose last record is fully at or before upToTxnId
//      (multi-segment retention; no single-file truncate-to-zero)
//
// Rotation: when the active segment reaches logFileMaxSize, it is fsync'd
// and a new segment file is created. Recovery streams every segment in
// order without loading the entire log into memory.
//
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "annpocketdb/core/page_manager.hpp"
#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"
#include "annpocketdb/wal/record.hpp"

namespace annpocketdb::wal {

struct WalOptions {
    // Soft max size of one segment before rotation (bytes).
    std::uint64_t logFileMaxSize = 64ull << 20;  // 64 MiB
    // Max segments retained after checkpoint (0 = delete all fully covered).
    std::uint32_t minSegmentsToKeep = 1;
    // Read chunk size for streaming recovery.
    std::uint32_t recoveryReadChunk = 1u << 20;  // 1 MiB
};

struct RecoveryResult {
    TxnId lastCommitTxnId = kInvalidTxnId;
    PageId lastMasterRoot = kInvalidPageId;
    PageId lastFreeListRoot = kInvalidPageId;
    SequenceNumber lastWalSequence = 0;
    std::uint32_t lastPageSize = 0;
    bool appliedCommit = false;
    std::uint64_t validLogBytes = 0;
    TxnId lastCheckpointTxnId = kInvalidTxnId;
    std::uint32_t segmentsScanned = 0;
};

class Wal {
public:
    // `path` is the WAL *directory* (created if missing). Conventionally
    // dataPath + ".wal".
    static Result<std::unique_ptr<Wal>> open(const std::filesystem::path& path,
                                              const WalOptions& options = {});

    ~Wal();
    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;
    Wal(Wal&&) noexcept;
    Wal& operator=(Wal&&) noexcept;

    Status logPageImage(PageId pageId, const core::PageHeader* header,
                        ByteView payload);
    Status logFreeListUpdate(const std::vector<PageId>& retiredPages,
                              TxnId retiringTxnId);
    Status commit(TxnId txnId, PageId masterRoot,
                  PageId freeListRoot, SequenceNumber walSequence,
                  core::SyncMode mode);

    // Append Checkpoint, fsync, remove segments fully covered by upToTxnId.
    Status checkpoint(TxnId upToTxnId, core::SyncMode mode);

    // Streaming multi-segment recovery. Do not attachWal before this.
    Result<RecoveryResult> recover(core::PageManager& pm);
    Result<TxnId> replayInto(core::PageManager& pm);

    // After a recovery that installed meta: drop all segments and start fresh.
    Status truncateAfterRecovery();

    Status flush(bool sync);

    std::uint64_t logFileSize() const;           // sum of all segment sizes
    std::uint32_t segmentCount() const { return static_cast<std::uint32_t>(segments_.size()); }
    bool healthy() const { return healthy_; }
    const std::filesystem::path& path() const { return dir_; }

private:
    Wal() = default;

    struct Segment {
        std::uint64_t id = 0;                     // monotonic
        std::filesystem::path path;
#if defined(_WIN32)
        void* handle = nullptr;                   // HANDLE
#else
        int fd = -1;
#endif
        std::uint64_t size = 0;                   // bytes on disk
        TxnId lastCommitTxnId = kInvalidTxnId;  // highest Commit seen in this seg
    };

    Status openDir(const std::filesystem::path& dir, bool create);
    Status discoverSegments();
    Status openSegmentForAppend(Segment& seg, bool create);
    Status closeSegment(Segment& seg);
    Status rotateSegment();
    Status appendBytes(const std::byte* data, std::size_t len, bool sync);
    Status flushBuffer(bool sync);
    Status ensureActiveSegment();

    // Streaming scan helpers
    struct StreamCursor {
        std::size_t segIndex = 0;
        std::uint64_t fileOffset = 0;
        ByteBuffer window;
        std::size_t windowPos = 0;
    };
    Result<bool> streamFill(StreamCursor& c, std::size_t need);
    // Returns false on clean EOF (truncated tail).
    Result<bool> streamNextRecord(StreamCursor& c, RecordType& type, ByteBuffer& body);

    static std::string segmentFileName(std::uint64_t id);
    static bool parseSegmentFileName(const std::string& name, std::uint64_t& id);

    std::filesystem::path dir_;
    WalOptions options_;
    std::vector<Segment> segments_;
    ByteBuffer writeBuffer_;
    bool healthy_ = true;
    TxnId recoveredTxnId_ = kInvalidTxnId;
};

}  // namespace annpocketdb::wal
