#include "annpocketdb/platform/mapped_file.hpp"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>  // FSCTL_SET_SPARSE
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace annpocketdb::platform {

namespace {
constexpr std::uint64_t kAllocationGranularity = 1ull << 20;  // 1 MiB
std::uint64_t roundUp(std::uint64_t value, std::uint64_t granularity) {
    return ((value + granularity - 1) / granularity) * granularity;
}
}  // namespace

MappedFile::~MappedFile() {
    if (isOpen()) (void)close();
}

MappedFile::MappedFile(MappedFile&& other) noexcept { *this = std::move(other); }

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this == &other) return *this;
    if (isOpen()) (void)close();
    base_ = other.base_;
    mappedSize_ = other.mappedSize_;
    fileSize_ = other.fileSize_;
    path_ = std::move(other.path_);
    mode_ = other.mode_;
#if defined(_WIN32)
    fileHandle_ = other.fileHandle_;
    mappingHandle_ = other.mappingHandle_;
    other.fileHandle_ = nullptr;
    other.mappingHandle_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
    other.base_ = nullptr;
    other.mappedSize_ = 0;
    other.fileSize_ = 0;
    return *this;
}

void MappedFile::reset() {
    base_ = nullptr;
    mappedSize_ = 0;
    fileSize_ = 0;
#if defined(_WIN32)
    fileHandle_ = nullptr;
    mappingHandle_ = nullptr;
#else
    fd_ = -1;
#endif
}

#if !defined(_WIN32)
// -------------------------- POSIX implementation --------------------------

Result<MappedFile> MappedFile::open(const std::filesystem::path& path, OpenMode mode,
                                     std::uint64_t initialSize, std::uint64_t maxSize) {
    MappedFile mf;
    mf.path_ = path;
    mf.mode_ = mode;

    const int flags = (mode == OpenMode::ReadOnly) ? O_RDONLY : (O_RDWR | O_CREAT);
    int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) return Status::IOError("open(" + path.string() + ") failed: " + std::strerror(errno));

    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return Status::IOError("fstat failed: " + std::string(std::strerror(errno)));
    }

    std::uint64_t currentSize = static_cast<std::uint64_t>(st.st_size);
    std::uint64_t targetSize = std::max<std::uint64_t>(currentSize, initialSize);
    if (mode == OpenMode::ReadWrite && targetSize > currentSize) {
        if (::ftruncate(fd, static_cast<off_t>(targetSize)) != 0) {
            ::close(fd);
            return Status::IOError("ftruncate failed: " + std::string(std::strerror(errno)));
        }
        currentSize = targetSize;
    }

    // Reserve the entire maxSize as virtual address space up front. This is
    // free physically; it only costs a range of virtual addresses. Growing
    // the file later becomes ftruncate-only (see resize()), so a pointer a
    // reader already holds into the mapping is never invalidated by another
    // thread's growth.
    std::uint64_t mapSize = roundUp(std::max<std::uint64_t>({currentSize, maxSize, 1}), kAllocationGranularity);

    int prot = (mode == OpenMode::ReadOnly) ? PROT_READ : (PROT_READ | PROT_WRITE);
    void* addr = ::mmap(nullptr, mapSize, prot, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        ::close(fd);
        return Status::IOError("mmap failed: " + std::string(std::strerror(errno)));
    }

    mf.fd_ = fd;
    mf.base_ = addr;
    mf.mappedSize_ = mapSize;
    mf.fileSize_ = currentSize;
    return Result<MappedFile>(std::move(mf));
}

Status MappedFile::expandMap(std::uint64_t newMappedSize) {
    if (base_ == nullptr) return Status::InvalidArgument("MappedFile not open");
    if (newMappedSize <= mappedSize_) return Status::OK();  // already large enough
    return remap(newMappedSize);
}

Status MappedFile::remap(std::uint64_t newMappedSize) {
    // Only used as a fallback if a resize() is requested beyond the
    // originally reserved address space (i.e. beyond maxDatabaseSize). Not
    // part of the normal growth path; callers should size maxDatabaseSize
    // generously since this path is not pointer-stable across concurrent
    // readers holding raw pointers into the old mapping.
#if defined(__linux__)
    void* newAddr = ::mremap(base_, mappedSize_, newMappedSize, MREMAP_MAYMOVE);
    if (newAddr == MAP_FAILED) return Status::IOError("mremap failed: " + std::string(std::strerror(errno)));
    base_ = newAddr;
    mappedSize_ = newMappedSize;
    return Status::OK();
#else
    if (::munmap(base_, mappedSize_) != 0)
        return Status::IOError("munmap failed during remap: " + std::string(std::strerror(errno)));
    int prot = (mode_ == OpenMode::ReadOnly) ? PROT_READ : (PROT_READ | PROT_WRITE);
    void* addr = ::mmap(nullptr, newMappedSize, prot, MAP_SHARED, fd_, 0);
    if (addr == MAP_FAILED) {
        base_ = nullptr;
        return Status::IOError("mmap failed during remap: " + std::string(std::strerror(errno)));
    }
    base_ = addr;
    mappedSize_ = newMappedSize;
    return Status::OK();
#endif
}

Status MappedFile::resize(std::uint64_t newSize) {
    if (mode_ == OpenMode::ReadOnly) return Status::ReadOnly("cannot resize a read-only mapped file");
    if (newSize <= fileSize_) {
        fileSize_ = std::max(fileSize_, newSize);
        return Status::OK();
    }
    if (newSize > mappedSize_) {
        // Exceeds the reserved address space established at open() time.
        // Rather than silently remapping (unsafe for concurrent readers),
        // surface this so the caller can report MapFull / ask the operator
        // to reopen with a larger maxDatabaseSize.
        return Status::MapFull("requested size " + std::to_string(newSize) +
                                " exceeds reserved map size " + std::to_string(mappedSize_));
    }
    if (::ftruncate(fd_, static_cast<off_t>(newSize)) != 0) {
        if (errno == ENOSPC) {
            return Status::DiskFull("ftruncate failed: " + std::string(std::strerror(errno)));
        }
        return Status::IOError("ftruncate failed: " + std::string(std::strerror(errno)));
    }
    fileSize_ = newSize;
    return Status::OK();
}

Status MappedFile::flush(std::uint64_t offset, std::uint64_t length, bool sync) const {
    if (!base_) return Status::IOError("flush on unopened file");
    static const long kPageSize = sysconf(_SC_PAGESIZE);
    std::uint64_t alignedOffset = (offset / static_cast<std::uint64_t>(kPageSize)) * static_cast<std::uint64_t>(kPageSize);
    std::uint64_t alignedLength = (offset - alignedOffset) + length;
    void* addr = static_cast<std::byte*>(base_) + alignedOffset;
    int flags = sync ? MS_SYNC : MS_ASYNC;
    if (::msync(addr, alignedLength, flags) != 0) {
        return Status::IOError("msync failed: " + std::string(std::strerror(errno)));
    }
    return Status::OK();
}

void MappedFile::adviseRandom(std::uint64_t offset, std::uint64_t length) const noexcept {
    if (base_) ::madvise(static_cast<std::byte*>(base_) + offset, length, MADV_RANDOM);
}
void MappedFile::adviseSequential(std::uint64_t offset, std::uint64_t length) const noexcept {
    if (base_) ::madvise(static_cast<std::byte*>(base_) + offset, length, MADV_SEQUENTIAL);
}
void MappedFile::adviseWillNeed(std::uint64_t offset, std::uint64_t length) const noexcept {
    if (base_) ::madvise(static_cast<std::byte*>(base_) + offset, length, MADV_WILLNEED);
}

Status MappedFile::close() {
    Status result = Status::OK();
    if (base_) {
        if (::munmap(base_, mappedSize_) != 0)
            result = Status::IOError("munmap failed: " + std::string(std::strerror(errno)));
    }
    if (fd_ >= 0) ::close(fd_);
    reset();
    return result;
}

#else
// -------------------------- Windows implementation -------------------------
// Pointer-stable growth: at open(), mark the file sparse and create a section
// + view covering the full maxSize reservation. resize() only advances the
// logical fileSize_ and never UnmapViewOfFile/MapViewOfFile, so concurrent
// readers holding PageHeader* remain valid — matching the POSIX model.

Result<MappedFile> MappedFile::open(const std::filesystem::path& path, OpenMode mode,
                                     std::uint64_t initialSize, std::uint64_t maxSize) {
    MappedFile mf;
    mf.path_ = path;
    mf.mode_ = mode;

    DWORD access = (mode == OpenMode::ReadOnly) ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
    DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;
    DWORD disposition = (mode == OpenMode::ReadOnly) ? OPEN_EXISTING : OPEN_ALWAYS;

    HANDLE file = CreateFileW(path.c_str(), access, shareMode, nullptr, disposition,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return Status::IOError("CreateFile failed for " + path.string() +
                               " (GetLastError=" + std::to_string(GetLastError()) + ")");
    }

    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    std::uint64_t currentSize = static_cast<std::uint64_t>(size.QuadPart);
    std::uint64_t targetSize = std::max<std::uint64_t>(currentSize, initialSize);

    if (mode == OpenMode::ReadWrite && targetSize > currentSize) {
        LARGE_INTEGER newSize; newSize.QuadPart = static_cast<LONGLONG>(targetSize);
        SetFilePointerEx(file, newSize, nullptr, FILE_BEGIN);
        SetEndOfFile(file);
        currentSize = targetSize;
    }

    // Reserve the full maxSize as the section/view size so growth never remaps.
    // CreateFileMapping requires the file to be at least the section size for
    // PAGE_READWRITE; mark sparse first so the extension does not allocate
    // full physical clusters (NTFS zeros are sparse until written).
    std::uint64_t mapSize = roundUp(
        std::max<std::uint64_t>({currentSize, maxSize, 1}), kAllocationGranularity);

    if (mode == OpenMode::ReadWrite) {
        DWORD bytesReturned = 0;
        // Best-effort: non-NTFS volumes may reject sparse; still try the extend.
        (void)DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                              &bytesReturned, nullptr);
        if (mapSize > currentSize) {
            LARGE_INTEGER newSize; newSize.QuadPart = static_cast<LONGLONG>(mapSize);
            if (!SetFilePointerEx(file, newSize, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) {
                DWORD err = GetLastError();
                CloseHandle(file);
                if (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL) {
                    return Status::DiskFull("SetEndOfFile to reservation size failed: disk full");
                }
                return Status::IOError("SetEndOfFile to reservation size failed");
            }
            // OS file size is now mapSize (sparse); logical fileSize_ stays at
            // currentSize so PageManager growth accounting is unchanged.
        }
    } else {
        // Read-only: cannot extend; map only what exists (no writer growth).
        mapSize = roundUp(std::max<std::uint64_t>(currentSize, 1), kAllocationGranularity);
    }

    DWORD protect = (mode == OpenMode::ReadOnly) ? PAGE_READONLY : PAGE_READWRITE;
    HANDLE mapping = CreateFileMappingW(file, nullptr, protect,
                                         static_cast<DWORD>(mapSize >> 32),
                                         static_cast<DWORD>(mapSize & 0xffffffffu), nullptr);
    if (!mapping) {
        DWORD err = GetLastError();
        CloseHandle(file);
        return Status::IOError("CreateFileMapping failed (GetLastError=" + std::to_string(err) +
                               ", mapSize=" + std::to_string(mapSize) + ")");
    }

    DWORD mapAccess = (mode == OpenMode::ReadOnly) ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS;
    void* addr = MapViewOfFile(mapping, mapAccess, 0, 0, static_cast<SIZE_T>(mapSize));
    if (!addr) {
        DWORD err = GetLastError();
        CloseHandle(mapping);
        CloseHandle(file);
        return Status::IOError("MapViewOfFile failed (GetLastError=" + std::to_string(err) +
                               ", mapSize=" + std::to_string(mapSize) + ")");
    }

    mf.fileHandle_ = file;
    mf.mappingHandle_ = mapping;
    mf.base_ = addr;
    mf.mappedSize_ = mapSize;
    mf.fileSize_ = currentSize;  // logical high-water; OS sparse size may be larger
    return Result<MappedFile>(std::move(mf));
}

Status MappedFile::expandMap(std::uint64_t newMappedSize) {
    if (base_ == nullptr) return Status::InvalidArgument("MappedFile not open");
    if (newMappedSize <= mappedSize_) return Status::OK();
    return Status::MapFull(
        "Windows expandMap requires reopen with a larger maxDatabaseSize "
        "(current reservation " + std::to_string(mappedSize_) + ")");
}

Status MappedFile::remap(std::uint64_t /*newMappedSize*/) {
    // Growth is pointer-stable: the full reservation is mapped at open().
    // Remap is intentionally unsupported — callers must size maxDatabaseSize
    // so resize() never needs a larger view.
    return Status::MapFull("Windows remap disabled; reopen with a larger maxDatabaseSize");
}

Status MappedFile::resize(std::uint64_t newSize) {
    if (mode_ == OpenMode::ReadOnly) return Status::ReadOnly("cannot resize a read-only mapped file");
    if (newSize <= fileSize_) {
        fileSize_ = std::max(fileSize_, newSize);
        return Status::OK();
    }
    if (newSize > mappedSize_) {
        // Exceeds the address-space reservation established at open(). Same
        // contract as POSIX: never silently remap under concurrent readers.
        return Status::MapFull("requested size " + std::to_string(newSize) +
                                " exceeds reserved map size " + std::to_string(mappedSize_));
    }
    // File was already extended to mappedSize_ (sparse) at open(); advancing
    // the logical high-water mark is enough. No UnmapViewOfFile.
    fileSize_ = newSize;
    return Status::OK();
}

Status MappedFile::flush(std::uint64_t offset, std::uint64_t length, bool sync) const {
    if (!base_) return Status::IOError("flush on unopened file");
    if (!FlushViewOfFile(static_cast<std::byte*>(base_) + offset, static_cast<SIZE_T>(length)))
        return Status::IOError("FlushViewOfFile failed");
    if (sync && !FlushFileBuffers(reinterpret_cast<HANDLE>(fileHandle_)))
        return Status::IOError("FlushFileBuffers failed");
    return Status::OK();
}

void MappedFile::adviseRandom(std::uint64_t, std::uint64_t) const noexcept {}
void MappedFile::adviseSequential(std::uint64_t, std::uint64_t) const noexcept {}
void MappedFile::adviseWillNeed(std::uint64_t offset, std::uint64_t length) const noexcept {
    if (!base_) return;
    WIN32_MEMORY_RANGE_ENTRY entry{static_cast<std::byte*>(base_) + offset, static_cast<SIZE_T>(length)};
    PrefetchVirtualMemory(GetCurrentProcess(), 1, &entry, 0);
}

Status MappedFile::close() {
    Status result = Status::OK();
    if (base_ && !UnmapViewOfFile(base_)) result = Status::IOError("UnmapViewOfFile failed");
    if (mappingHandle_) CloseHandle(reinterpret_cast<HANDLE>(mappingHandle_));
    if (fileHandle_) CloseHandle(reinterpret_cast<HANDLE>(fileHandle_));
    reset();
    return result;
}

#endif

}  // namespace annpocketdb::platform
