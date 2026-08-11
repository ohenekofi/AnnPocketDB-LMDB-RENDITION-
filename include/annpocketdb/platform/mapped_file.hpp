#pragma once
//
// AnnPocketDb :: platform/mapped_file.hpp
//
// Thin, portable wrapper around "open a file, memory-map it, grow it, flush
// it, close it". This is the *only* place in the engine that touches raw OS
// file/mmap APIs.
//
// Growth model (POSIX and Windows): the full `maxSize` address-space
// reservation is established once at open() time so growing the logical
// file afterward never remaps and never invalidates pointers a reader
// already holds into the mapping. This mirrors LMDB's fixed-mapsize design.
//
// On POSIX the reservation is a pure virtual mmap of maxSize; physical pages
// are faulted only as the file grows via ftruncate.
//
// On Windows, CreateFileMapping requires the backing file to be at least the
// section size, so open() marks the file sparse (NTFS) and extends it to
// maxSize before mapping. Sparse extension does not consume full disk space;
// only written regions allocate clusters. resize() then only advances the
// in-memory logical fileSize_ (and never calls UnmapViewOfFile) so reader
// PageHeader* pointers remain valid across writer growth — the same
// pointer-stability guarantee as POSIX. Exceeding the reservation returns
// Status::MapFull.
//
#include <cstdint>
#include <filesystem>
#include <string>

#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

namespace annpocketdb::platform {

enum class OpenMode { ReadOnly, ReadWrite };

class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    // Opens (creating if necessary) `path`. Reserves `maxSize` bytes of
    // address space and ensures the file is at least `initialSize` bytes.
    static Result<MappedFile> open(const std::filesystem::path& path,
                                    OpenMode mode,
                                    std::uint64_t initialSize,
                                    std::uint64_t maxSize);

    // Grows the underlying file (ftruncate only -- no remap) to at least
    // `newSize` bytes, provided newSize <= the reserved mapping size.
    // Returns Status::MapFull if newSize would exceed the reservation.
    Status resize(std::uint64_t newSize);
    // Expand the reserved mapping (may move base address). Caller must
    // guarantee no concurrent use of prior pointers into the map.
    Status expandMap(std::uint64_t newMappedSize);
    std::uint64_t maxMappedSize() const { return mappedSize_; }

    Status flush(std::uint64_t offset, std::uint64_t length, bool sync) const;

    void adviseRandom(std::uint64_t offset, std::uint64_t length) const noexcept;
    void adviseSequential(std::uint64_t offset, std::uint64_t length) const noexcept;
    void adviseWillNeed(std::uint64_t offset, std::uint64_t length) const noexcept;

    Status close();

    std::byte* data() { return static_cast<std::byte*>(base_); }
    const std::byte* data() const { return static_cast<const std::byte*>(base_); }
    std::uint64_t mappedSize() const { return mappedSize_; }
    std::uint64_t fileSize() const { return fileSize_; }
    bool isOpen() const { return base_ != nullptr; }
    const std::filesystem::path& path() const { return path_; }

private:
    void* base_ = nullptr;
    std::uint64_t mappedSize_ = 0;
    std::uint64_t fileSize_ = 0;
    std::filesystem::path path_;
    OpenMode mode_ = OpenMode::ReadWrite;

#if defined(_WIN32)
    void* fileHandle_ = nullptr;
    void* mappingHandle_ = nullptr;
#else
    int fd_ = -1;
#endif

    Status remap(std::uint64_t newMappedSize);
    void reset();
};

}  // namespace annpocketdb::platform
