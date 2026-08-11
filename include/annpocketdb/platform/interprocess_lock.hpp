#pragma once
//
// AnnPocketDb :: platform/interprocess_lock.hpp
//
// LMDB-style multi-process coordination for a single database file:
//
//   data.apdb          — memory-mapped data (unchanged)
//   data.apdb.lock     — sidecar lock file holding:
//                          * exclusive writer lock (fcntl / LockFileEx)
//                          * shared-memory reader-slot table (mmap)
//
// Writer exclusion is cross-process: only one WriteTransaction may be active
// across all processes. Reader slots are visible to every process so the
// writer's free-list reclaim sees the oldest *global* snapshot, not just
// this process's readers.
//
// Dead-process recovery: each slot records the registering process id.
// registerReader() / oldestActiveSnapshot() opportunistically clear slots
// whose pid is no longer alive (crash without unregister).
//
#include <atomic>
#include <vector>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

namespace annpocketdb::platform {

using ReaderHandle = std::uint64_t;
constexpr ReaderHandle kInvalidReaderHandle = static_cast<ReaderHandle>(-1);

// Fixed slot count in the shared table (same order of magnitude as LMDB's
// default reader slots; overflow returns Busy rather than a secondary map
// because secondary maps cannot live in a fixed shared mapping).
constexpr std::size_t kSharedReaderSlots = 256;

// Atomics must not live under #pragma pack — keep natural alignment and
// pad each slot to a cache line to avoid false sharing across processes.
struct alignas(64) SharedReaderSlot {
    std::atomic<TxnId> txnId{kInvalidTxnId};  // 0 = empty
    std::atomic<std::uint64_t> pid{0};
};
static_assert(sizeof(SharedReaderSlot) == 64, "SharedReaderSlot cache-line sized");

struct SharedLockHeader {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t slotCount = 0;
    std::uint32_t reserved = 0;
};

constexpr std::uint32_t kLockFileMagic = 0x41504C4Bu;  // "APLK"
constexpr std::uint32_t kLockFileVersion = 1;

// Size of the mapped lock-file region.
inline constexpr std::size_t sharedLockMapBytes() {
    return sizeof(SharedLockHeader) + kSharedReaderSlots * sizeof(SharedReaderSlot);
}

// Cross-process lock + shared reader table bound to one database path.
class InterprocessLock {
public:
    InterprocessLock() = default;
    ~InterprocessLock();

    InterprocessLock(const InterprocessLock&) = delete;
    InterprocessLock& operator=(const InterprocessLock&) = delete;
    InterprocessLock(InterprocessLock&&) noexcept;
    InterprocessLock& operator=(InterprocessLock&&) noexcept;

    // Opens (or creates) `dataPath + ".lock"`. readOnly still maps the shared
    // table so this process can register readers, but refuses writer locks.
    static Result<InterprocessLock> open(const std::filesystem::path& dataPath, bool readOnly);

    // ---- Writer exclusion (BasicLockable for std::unique_lock) ----
    void lock();
    void unlock();
    bool try_lock();

    // ---- Shared reader table ----
    ReaderHandle registerReader(TxnId snapshotTxnId);
    void unregisterReader(ReaderHandle handle);
    TxnId oldestActiveSnapshot(TxnId fallbackIfNoneActive) const;
    std::size_t activeReaderCount() const;
    static constexpr std::size_t maxReaders() { return kSharedReaderSlots; }

    // Clear slots whose pid is dead. Returns number of slots reclaimed.
    std::size_t readerCheck();
    // Occupied slots (after an optional internal check of dead pids).
    struct ReaderSlotInfo {
        std::uint32_t slot = 0;
        std::uint64_t pid = 0;
        TxnId snapshotTxnId = 0;
    };
    std::vector<ReaderSlotInfo> readerList() const;

    bool valid() const { return mapped_ != nullptr; }
    const std::filesystem::path& path() const { return path_; }

private:
    void reset() noexcept;
    // Sweeps every slot whose registering pid is no longer alive, reclaiming
    // it. Returns the number of slots reclaimed. Shared by the public
    // readerCheck() (explicit, reports the count) and the opportunistic
    // calls inside registerReader()/oldestActiveSnapshot() (which discard
    // the count) -- previously two separately-written copies of this sweep.
    std::size_t clearDeadSlots() const;
    static bool processAlive(std::uint64_t pid);

    std::filesystem::path path_;
    bool readOnly_ = false;
    void* mapped_ = nullptr;
    std::size_t mappedBytes_ = 0;

#if defined(_WIN32)
    void* fileHandle_ = nullptr;
    void* mappingHandle_ = nullptr;
    bool writerLocked_ = false;
#else
    int fd_ = -1;
    bool writerLocked_ = false;
#endif

    // Serializes threads *within* this process before taking the OS lock so
    // two threads do not both believe they hold the exclusive writer lock.
    std::mutex processMutex_;

    SharedLockHeader* header() const {
        return static_cast<SharedLockHeader*>(mapped_);
    }
    SharedReaderSlot* slots() const {
        return reinterpret_cast<SharedReaderSlot*>(
            static_cast<std::byte*>(mapped_) + sizeof(SharedLockHeader));
    }
};

}  // namespace annpocketdb::platform
