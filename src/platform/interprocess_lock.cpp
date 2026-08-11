#include "annpocketdb/platform/interprocess_lock.hpp"

#include <cstring>
#include <string>

#if defined(_WIN32)
#  define NOMINMAX
#  include <windows.h>
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/file.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace annpocketdb::platform {

namespace {
std::filesystem::path lockPathFor(const std::filesystem::path& dataPath) {
    return dataPath.string() + ".lock";
}
}  // namespace

InterprocessLock::~InterprocessLock() {
    if (writerLocked_) {
        try {
            unlock();
        } catch (...) {
        }
    }
    reset();
}

InterprocessLock::InterprocessLock(InterprocessLock&& o) noexcept {
    *this = std::move(o);
}

InterprocessLock& InterprocessLock::operator=(InterprocessLock&& o) noexcept {
    if (this == &o) return *this;
    if (writerLocked_) {
        try {
            unlock();
        } catch (...) {
        }
    }
    reset();
    path_ = std::move(o.path_);
    readOnly_ = o.readOnly_;
    mapped_ = o.mapped_;
    mappedBytes_ = o.mappedBytes_;
#if defined(_WIN32)
    fileHandle_ = o.fileHandle_;
    mappingHandle_ = o.mappingHandle_;
    o.fileHandle_ = nullptr;
    o.mappingHandle_ = nullptr;
#else
    fd_ = o.fd_;
    o.fd_ = -1;
#endif
    writerLocked_ = o.writerLocked_;
    o.mapped_ = nullptr;
    o.mappedBytes_ = 0;
    o.writerLocked_ = false;
    return *this;
}

void InterprocessLock::reset() noexcept {
#if defined(_WIN32)
    if (mapped_) {
        UnmapViewOfFile(mapped_);
        mapped_ = nullptr;
    }
    if (mappingHandle_) {
        CloseHandle(static_cast<HANDLE>(mappingHandle_));
        mappingHandle_ = nullptr;
    }
    if (fileHandle_) {
        CloseHandle(static_cast<HANDLE>(fileHandle_));
        fileHandle_ = nullptr;
    }
#else
    if (mapped_ && mapped_ != MAP_FAILED) {
        munmap(mapped_, mappedBytes_);
        mapped_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
    mappedBytes_ = 0;
}


std::size_t InterprocessLock::readerCheck() {
    return clearDeadSlots();
}

std::vector<InterprocessLock::ReaderSlotInfo> InterprocessLock::readerList() const {
    std::vector<ReaderSlotInfo> out;
    if (!mapped_) return out;
    const SharedReaderSlot* s = slots();
    for (std::size_t i = 0; i < kSharedReaderSlots; ++i) {
        TxnId txn = s[i].txnId.load(std::memory_order_acquire);
        if (txn == 0) continue;
        ReaderSlotInfo info;
        info.slot = static_cast<std::uint32_t>(i);
        info.pid = s[i].pid.load(std::memory_order_acquire);
        info.snapshotTxnId = txn;
        out.push_back(info);
    }
    return out;
}

bool InterprocessLock::processAlive(std::uint64_t pid) {
    if (pid == 0) return false;
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD code = 0;
    BOOL ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return ok && code == STILL_ACTIVE;
#else
    // signal 0: existence check. ESRCH => dead. EPERM => alive but not ours.
    if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno == EPERM;
#endif
}

std::size_t InterprocessLock::clearDeadSlots() const {
    if (!mapped_) return 0;
    SharedReaderSlot* s = slots();
    std::size_t cleared = 0;
    for (std::size_t i = 0; i < kSharedReaderSlots; ++i) {
        TxnId t = s[i].txnId.load(std::memory_order_acquire);
        if (t == kInvalidTxnId) continue;
        std::uint64_t pid = s[i].pid.load(std::memory_order_acquire);
        if (!processAlive(pid)) {
            // Best-effort reclaim of a crashed reader's slot.
            s[i].txnId.store(kInvalidTxnId, std::memory_order_release);
            s[i].pid.store(0, std::memory_order_release);
            ++cleared;
        }
    }
    return cleared;
}

#if !defined(_WIN32)
// ---------------- POSIX ----------------

Result<InterprocessLock> InterprocessLock::open(const std::filesystem::path& dataPath, bool readOnly) {
    InterprocessLock lk;
    lk.path_ = lockPathFor(dataPath);
    lk.readOnly_ = readOnly;

    const int flags = readOnly ? O_RDWR : (O_RDWR | O_CREAT);  // need write for atomic slot CAS
    int fd = ::open(lk.path_.c_str(), flags, 0644);
    if (fd < 0) {
        return Status::IOError("open lock file failed: " + std::string(std::strerror(errno)));
    }

    const std::size_t mapBytes = sharedLockMapBytes();
    if (!readOnly) {
        if (::ftruncate(fd, static_cast<off_t>(mapBytes)) != 0) {
            ::close(fd);
            return Status::IOError("ftruncate lock file failed: " + std::string(std::strerror(errno)));
        }
    } else {
        struct stat st{};
        if (::fstat(fd, &st) != 0 || static_cast<std::size_t>(st.st_size) < mapBytes) {
            ::close(fd);
            return Status::IOError("lock file missing or too small for read-only open");
        }
    }

    void* addr = ::mmap(nullptr, mapBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        ::close(fd);
        return Status::IOError("mmap lock file failed: " + std::string(std::strerror(errno)));
    }

    lk.fd_ = fd;
    lk.mapped_ = addr;
    lk.mappedBytes_ = mapBytes;

    auto* hdr = lk.header();
    if (hdr->magic == 0) {
        // First initializer wins; concurrent first-opens may race but the
        // values written are identical constants.
        hdr->magic = kLockFileMagic;
        hdr->version = kLockFileVersion;
        hdr->slotCount = static_cast<std::uint32_t>(kSharedReaderSlots);
        for (std::size_t i = 0; i < kSharedReaderSlots; ++i) {
            lk.slots()[i].txnId.store(kInvalidTxnId, std::memory_order_relaxed);
            lk.slots()[i].pid.store(0, std::memory_order_relaxed);
        }
    } else if (hdr->magic != kLockFileMagic || hdr->version != kLockFileVersion) {
        lk.reset();
        return Status::Incompatible("unsupported lock file format");
    }

    return Result<InterprocessLock>(std::move(lk));
}

void InterprocessLock::lock() {
    processMutex_.lock();
    if (readOnly_) {
        processMutex_.unlock();
        throw std::runtime_error("writer lock requested on read-only interprocess lock");
    }
    // Exclusive flock on the whole lock file.
    while (::flock(fd_, LOCK_EX) != 0) {
        if (errno == EINTR) continue;
        processMutex_.unlock();
        throw std::runtime_error(std::string("flock LOCK_EX failed: ") + std::strerror(errno));
    }
    writerLocked_ = true;
}

bool InterprocessLock::try_lock() {
    if (!processMutex_.try_lock()) return false;
    if (readOnly_) {
        processMutex_.unlock();
        return false;
    }
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        processMutex_.unlock();
        return false;
    }
    writerLocked_ = true;
    return true;
}

void InterprocessLock::unlock() {
    if (writerLocked_) {
        ::flock(fd_, LOCK_UN);
        writerLocked_ = false;
    }
    processMutex_.unlock();
}

#else
// ---------------- Windows ----------------

Result<InterprocessLock> InterprocessLock::open(const std::filesystem::path& dataPath, bool readOnly) {
    InterprocessLock lk;
    lk.path_ = lockPathFor(dataPath);
    lk.readOnly_ = readOnly;

    DWORD access = GENERIC_READ | GENERIC_WRITE;
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    DWORD disp = readOnly ? OPEN_EXISTING : OPEN_ALWAYS;

    HANDLE file = CreateFileW(lk.path_.c_str(), access, share, nullptr, disp,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return Status::IOError("CreateFile lock failed");
    }

    const std::size_t mapBytes = sharedLockMapBytes();
    if (!readOnly) {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(mapBytes);
        if (!SetFilePointerEx(file, li, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) {
            CloseHandle(file);
            return Status::IOError("SetEndOfFile lock failed");
        }
    }

    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE,
                                        0, static_cast<DWORD>(mapBytes), nullptr);
    if (!mapping) {
        CloseHandle(file);
        return Status::IOError("CreateFileMapping lock failed");
    }
    void* addr = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, mapBytes);
    if (!addr) {
        CloseHandle(mapping);
        CloseHandle(file);
        return Status::IOError("MapViewOfFile lock failed");
    }

    lk.fileHandle_ = file;
    lk.mappingHandle_ = mapping;
    lk.mapped_ = addr;
    lk.mappedBytes_ = mapBytes;

    auto* hdr = lk.header();
    if (hdr->magic == 0) {
        hdr->magic = kLockFileMagic;
        hdr->version = kLockFileVersion;
        hdr->slotCount = static_cast<std::uint32_t>(kSharedReaderSlots);
        for (std::size_t i = 0; i < kSharedReaderSlots; ++i) {
            lk.slots()[i].txnId.store(kInvalidTxnId, std::memory_order_relaxed);
            lk.slots()[i].pid.store(0, std::memory_order_relaxed);
        }
    } else if (hdr->magic != kLockFileMagic || hdr->version != kLockFileVersion) {
        lk.reset();
        return Status::Incompatible("unsupported lock file format");
    }

    return Result<InterprocessLock>(std::move(lk));
}

void InterprocessLock::lock() {
    processMutex_.lock();
    if (readOnly_) {
        processMutex_.unlock();
        throw std::runtime_error("writer lock requested on read-only interprocess lock");
    }
    OVERLAPPED ov{};
    // Exclusive lock on the first byte of the lock file.
    if (!LockFileEx(static_cast<HANDLE>(fileHandle_), LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov)) {
        processMutex_.unlock();
        throw std::runtime_error("LockFileEx failed");
    }
    writerLocked_ = true;
}

bool InterprocessLock::try_lock() {
    if (!processMutex_.try_lock()) return false;
    if (readOnly_) {
        processMutex_.unlock();
        return false;
    }
    OVERLAPPED ov{};
    if (!LockFileEx(static_cast<HANDLE>(fileHandle_),
                    LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
        processMutex_.unlock();
        return false;
    }
    writerLocked_ = true;
    return true;
}

void InterprocessLock::unlock() {
    if (writerLocked_) {
        OVERLAPPED ov{};
        UnlockFileEx(static_cast<HANDLE>(fileHandle_), 0, 1, 0, &ov);
        writerLocked_ = false;
    }
    processMutex_.unlock();
}
#endif

// ---- Shared reader table (common) ----

ReaderHandle InterprocessLock::registerReader(TxnId snapshotTxnId) {
    if (!mapped_) return kInvalidReaderHandle;
    clearDeadSlots();
#if defined(_WIN32)
    const std::uint64_t myPid = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    const std::uint64_t myPid = static_cast<std::uint64_t>(::getpid());
#endif
    SharedReaderSlot* s = slots();
    for (std::size_t i = 0; i < kSharedReaderSlots; ++i) {
        TxnId expected = kInvalidTxnId;
        if (s[i].txnId.compare_exchange_strong(expected, snapshotTxnId,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
            s[i].pid.store(myPid, std::memory_order_release);
            return static_cast<ReaderHandle>(i);
        }
    }
    // Table full — match LMDB's MDB_READERS_FULL behaviour rather than growing
    // an unbounded in-process map that other processes cannot see.
    return kInvalidReaderHandle;
}

void InterprocessLock::unregisterReader(ReaderHandle handle) {
    if (!mapped_ || handle == kInvalidReaderHandle) return;
    if (handle >= kSharedReaderSlots) return;
    SharedReaderSlot& slot = slots()[handle];
    slot.txnId.store(kInvalidTxnId, std::memory_order_release);
    slot.pid.store(0, std::memory_order_release);
}

TxnId InterprocessLock::oldestActiveSnapshot(TxnId fallbackIfNoneActive) const {
    if (!mapped_) return fallbackIfNoneActive;
    clearDeadSlots();
    bool any = false;
    TxnId oldest = kInvalidTxnId;
    SharedReaderSlot* s = slots();
    for (std::size_t i = 0; i < kSharedReaderSlots; ++i) {
        TxnId v = s[i].txnId.load(std::memory_order_acquire);
        if (v == kInvalidTxnId) continue;
        if (!any || v < oldest) oldest = v;
        any = true;
    }
    return any ? oldest : fallbackIfNoneActive;
}

std::size_t InterprocessLock::activeReaderCount() const {
    if (!mapped_) return 0;
    std::size_t n = 0;
    SharedReaderSlot* s = slots();
    for (std::size_t i = 0; i < kSharedReaderSlots; ++i) {
        if (s[i].txnId.load(std::memory_order_relaxed) != kInvalidTxnId) ++n;
    }
    return n;
}

}  // namespace annpocketdb::platform
