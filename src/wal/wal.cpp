#include "annpocketdb/wal/wal.hpp"
#include "annpocketdb/util/path.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dirent.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace annpocketdb::wal {

namespace {

// ---------------------------------------------------------------------------
// Platform file I/O
// ---------------------------------------------------------------------------

#if !defined(_WIN32)

using Fd = int;
constexpr Fd kInvalidFd = -1;

Status writeAll(Fd fd, const std::byte* data, std::size_t len) {
    while (len > 0) {
        ssize_t n = ::write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return Status::IOError(std::string("wal write: ") + std::strerror(errno));
        }
        data += n;
        len -= static_cast<std::size_t>(n);
    }
    return Status::OK();
}

Status fsyncFd(Fd fd) {
    if (::fsync(fd) != 0)
        return Status::IOError(std::string("wal fsync: ") + std::strerror(errno));
    return Status::OK();
}

Status openFd(const std::filesystem::path& path, bool create, bool append, Fd& out) {
    int flags = O_RDWR;
    if (create) flags |= O_CREAT;
    if (append) flags |= O_APPEND;
    int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) return Status::IOError(std::string("wal open: ") + std::strerror(errno));
    out = fd;
    return Status::OK();
}

Status openFdRead(const std::filesystem::path& path, Fd& out) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return Status::IOError(std::string("wal open read: ") + std::strerror(errno));
    out = fd;
    return Status::OK();
}

void closeFd(Fd fd) {
    if (fd >= 0) ::close(fd);
}

Status fileSizeFd(Fd fd, std::uint64_t& out) {
    struct stat st{};
    if (::fstat(fd, &st) != 0)
        return Status::IOError(std::string("wal fstat: ") + std::strerror(errno));
    out = static_cast<std::uint64_t>(st.st_size);
    return Status::OK();
}

Status preadFd(Fd fd, void* buf, std::size_t len, std::uint64_t offset, std::size_t& got) {
    got = 0;
    auto* p = static_cast<char*>(buf);
    while (got < len) {
        ssize_t n = ::pread(fd, p + got, len - got, static_cast<off_t>(offset + got));
        if (n < 0) {
            if (errno == EINTR) continue;
            return Status::IOError(std::string("wal pread: ") + std::strerror(errno));
        }
        if (n == 0) break;
        got += static_cast<std::size_t>(n);
    }
    return Status::OK();
}

Status removePath(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
    if (ec) return Status::IOError("wal remove: " + ec.message());
    return Status::OK();
}

#else  // _WIN32

using Fd = HANDLE;
// Not a valid constexpr initializer on MSVC: INVALID_HANDLE_VALUE is a
// C-style cast of an integer to a pointer, which constant-expression
// evaluation rejects even though the value is compile-time known.
const Fd kInvalidFd = INVALID_HANDLE_VALUE;

Status writeAll(Fd h, const std::byte* data, std::size_t len) {
    while (len > 0) {
        DWORD toWrite = static_cast<DWORD>(std::min<std::size_t>(len, 0x7FFFFFFFu));
        DWORD written = 0;
        if (!WriteFile(h, data, toWrite, &written, nullptr)) {
            return Status::IOError("wal WriteFile failed (" + std::to_string(GetLastError()) + ")");
        }
        data += written;
        len -= written;
    }
    return Status::OK();
}

Status fsyncFd(Fd h) {
    if (!FlushFileBuffers(h))
        return Status::IOError("wal FlushFileBuffers failed (" + std::to_string(GetLastError()) + ")");
    return Status::OK();
}

Status openFd(const std::filesystem::path& path, bool create, bool append, Fd& out) {
    DWORD access = GENERIC_READ | GENERIC_WRITE;
    DWORD share = FILE_SHARE_READ;
    DWORD disp = create ? OPEN_ALWAYS : OPEN_EXISTING;
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    HANDLE h = CreateFileW(path.wstring().c_str(), access, share, nullptr, disp, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return Status::IOError("wal CreateFile failed (" + std::to_string(GetLastError()) + ")");
    if (append) {
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(h, zero, nullptr, FILE_END)) {
            CloseHandle(h);
            return Status::IOError("wal seek end failed");
        }
    }
    out = h;
    return Status::OK();
}

Status openFdRead(const std::filesystem::path& path, Fd& out) {
    HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return Status::IOError("wal CreateFile read failed (" + std::to_string(GetLastError()) + ")");
    out = h;
    return Status::OK();
}

void closeFd(Fd h) {
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

Status fileSizeFd(Fd h, std::uint64_t& out) {
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz))
        return Status::IOError("wal GetFileSizeEx failed");
    out = static_cast<std::uint64_t>(sz.QuadPart);
    return Status::OK();
}

Status preadFd(Fd h, void* buf, std::size_t len, std::uint64_t offset, std::size_t& got) {
    got = 0;
    auto* p = static_cast<char*>(buf);
    while (got < len) {
        OVERLAPPED ov{};
        std::uint64_t off = offset + got;
        ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(off >> 32);
        DWORD toRead = static_cast<DWORD>(std::min<std::size_t>(len - got, 0x7FFFFFFFu));
        DWORD n = 0;
        if (!ReadFile(h, p + got, toRead, &n, &ov)) {
            DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF) break;
            return Status::IOError("wal ReadFile failed (" + std::to_string(err) + ")");
        }
        if (n == 0) break;
        got += n;
    }
    return Status::OK();
}

Status removePath(const std::filesystem::path& p) {
    if (!DeleteFileW(p.wstring().c_str())) {
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND)
            return Status::IOError("wal DeleteFile failed (" + std::to_string(err) + ")");
    }
    return Status::OK();
}

#endif

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

std::string Wal::segmentFileName(std::uint64_t id) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%08llu.seg", static_cast<unsigned long long>(id));
    return buf;
}

bool Wal::parseSegmentFileName(const std::string& name, std::uint64_t& id) {
    if (name.size() != 12 || name.substr(8) != ".seg") return false;
    for (int i = 0; i < 8; ++i)
        if (name[i] < '0' || name[i] > '9') return false;
    id = 0;
    for (int i = 0; i < 8; ++i) id = id * 10 + static_cast<std::uint64_t>(name[i] - '0');
    return true;
}

Wal::~Wal() {
    for (auto& s : segments_) closeSegment(s);
}

Wal::Wal(Wal&& o) noexcept
    : dir_(std::move(o.dir_)),
      options_(o.options_),
      segments_(std::move(o.segments_)),
      writeBuffer_(std::move(o.writeBuffer_)),
      healthy_(o.healthy_),
      recoveredTxnId_(o.recoveredTxnId_) {
    o.healthy_ = false;
}

Wal& Wal::operator=(Wal&& o) noexcept {
    if (this == &o) return *this;
    for (auto& s : segments_) closeSegment(s);
    dir_ = std::move(o.dir_);
    options_ = o.options_;
    segments_ = std::move(o.segments_);
    writeBuffer_ = std::move(o.writeBuffer_);
    healthy_ = o.healthy_;
    recoveredTxnId_ = o.recoveredTxnId_;
    o.healthy_ = false;
    return *this;
}

Status Wal::closeSegment(Segment& seg) {
#if defined(_WIN32)
    if (seg.handle) {
        closeFd(static_cast<Fd>(seg.handle));
        seg.handle = nullptr;
    }
#else
    if (seg.fd >= 0) {
        closeFd(seg.fd);
        seg.fd = -1;
    }
#endif
    return Status::OK();
}

Status Wal::openSegmentForAppend(Segment& seg, bool create) {
    Fd fd = kInvalidFd;
    Status s = openFd(seg.path, create, /*append=*/true, fd);
    if (!s.ok()) return s;
#if defined(_WIN32)
    seg.handle = fd;
#else
    seg.fd = fd;
#endif
    s = fileSizeFd(fd, seg.size);
    if (!s.ok()) {
        closeSegment(seg);
        return s;
    }
    return Status::OK();
}

Status Wal::discoverSegments() {
    segments_.clear();
    std::error_code ec;
    if (!std::filesystem::exists(dir_, ec)) return Status::OK();

    for (auto& ent : std::filesystem::directory_iterator(dir_, ec)) {
        if (ec) return Status::IOError("wal readdir: " + ec.message());
        if (!ent.is_regular_file()) continue;
        std::uint64_t id = 0;
        if (!parseSegmentFileName(util::pathToUtf8(ent.path().filename()), id)) continue;
        Segment seg;
        seg.id = id;
        seg.path = ent.path();
        segments_.push_back(std::move(seg));
    }
    std::sort(segments_.begin(), segments_.end(),
              [](const Segment& a, const Segment& b) { return a.id < b.id; });
    return Status::OK();
}

Result<std::unique_ptr<Wal>> Wal::open(const std::filesystem::path& path, const WalOptions& options) {
    auto wal = std::unique_ptr<Wal>(new Wal());
    wal->dir_ = path;
    wal->options_ = options;
    wal->writeBuffer_.reserve(1 << 20);

    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) return Status::IOError("wal mkdir: " + ec.message());

    Status s = wal->discoverSegments();
    if (!s.ok()) return s;

    if (wal->segments_.empty()) {
        Segment seg;
        seg.id = 1;
        seg.path = path / segmentFileName(1);
        s = wal->openSegmentForAppend(seg, /*create=*/true);
        if (!s.ok()) return s;
        wal->segments_.push_back(std::move(seg));
    } else {
        // Open the last segment for append; earlier segments stay closed until recovery.
        s = wal->openSegmentForAppend(wal->segments_.back(), /*create=*/false);
        if (!s.ok()) return s;
    }
    return Result<std::unique_ptr<Wal>>(std::move(wal));
}

Status Wal::ensureActiveSegment() {
    if (segments_.empty()) return Status::Internal("wal has no segments");
#if defined(_WIN32)
    if (segments_.back().handle == nullptr)
        return openSegmentForAppend(segments_.back(), false);
#else
    if (segments_.back().fd < 0) return openSegmentForAppend(segments_.back(), false);
#endif
    return Status::OK();
}

Status Wal::rotateSegment() {
    Status s = flushBuffer(true);
    if (!s.ok()) return s;
    if (!segments_.empty()) {
        s = closeSegment(segments_.back());
        if (!s.ok()) return s;
    }
    Segment seg;
    seg.id = segments_.empty() ? 1 : segments_.back().id + 1;
    seg.path = dir_ / segmentFileName(seg.id);
    s = openSegmentForAppend(seg, /*create=*/true);
    if (!s.ok()) return s;
    segments_.push_back(std::move(seg));
    return Status::OK();
}

Status Wal::appendBytes(const std::byte* data, std::size_t len, bool sync) {
    Status s = ensureActiveSegment();
    if (!s.ok()) return s;
    Segment& seg = segments_.back();
#if defined(_WIN32)
    Fd fd = static_cast<Fd>(seg.handle);
#else
    Fd fd = seg.fd;
#endif
    s = writeAll(fd, data, len);
    if (!s.ok()) {
        healthy_ = false;
        return s;
    }
    seg.size += len;
    if (sync) {
        s = fsyncFd(fd);
        if (!s.ok()) {
            healthy_ = false;
            return s;
        }
    }
    if (seg.size >= options_.logFileMaxSize) {
        s = rotateSegment();
        if (!s.ok()) return s;
    }
    return Status::OK();
}

Status Wal::flushBuffer(bool sync) {
    if (writeBuffer_.empty()) {
        if (sync && !segments_.empty()) {
            Status s = ensureActiveSegment();
            if (!s.ok()) return s;
#if defined(_WIN32)
            return fsyncFd(static_cast<Fd>(segments_.back().handle));
#else
            return fsyncFd(segments_.back().fd);
#endif
        }
        return Status::OK();
    }
    Status s = appendBytes(writeBuffer_.data(), writeBuffer_.size(), sync);
    writeBuffer_.clear();
    return s;
}

Status Wal::flush(bool sync) { return flushBuffer(sync); }

std::uint64_t Wal::logFileSize() const {
    std::uint64_t n = 0;
    for (const auto& s : segments_) n += s.size;
    return n + writeBuffer_.size();
}

// ---------------------------------------------------------------------------
// Record append
// ---------------------------------------------------------------------------

Status Wal::logPageImage(PageId pageId, const core::PageHeader* header, ByteView payload) {
    PageImageRecord rec;
    rec.pageId = pageId;
    rec.type = header->type;
    rec.payloadSize = header->payloadSize;
    rec.itemCount = header->itemCount;
    rec.flags = header->flags;
    rec.overflowNext = header->overflowNext;
    rec.payload.assign(payload.begin(), payload.end());
    Status s = encodePageImage(rec, static_cast<std::uint32_t>(sizeof(core::PageHeader) + payload.size()),
                               writeBuffer_);
    if (!s.ok()) return s;
    if (writeBuffer_.size() >= (1u << 20)) return flushBuffer(false);
    return Status::OK();
}

Status Wal::logFreeListUpdate(const std::vector<PageId>& retiredPages,
                               TxnId retiringTxnId) {
    FreeListUpdateRecord rec;
    rec.retiredPages = retiredPages;
    rec.retiringTxnId = retiringTxnId;
    Status s = encodeFreeListUpdate(rec, writeBuffer_);
    if (!s.ok()) return s;
    if (writeBuffer_.size() >= (1u << 20)) return flushBuffer(false);
    return Status::OK();
}

Status Wal::commit(TxnId txnId, PageId masterRoot, PageId freeListRoot,
                   SequenceNumber walSequence, core::SyncMode mode) {
    CommitRecord rec;
    rec.txnId = txnId;
    rec.masterRootPageId = masterRoot;
    rec.freeListRootPageId = freeListRoot;
    rec.walSequence = walSequence;
    rec.pageSize = 0;
    Status s = encodeCommit(rec, writeBuffer_);
    if (!s.ok()) return s;
    bool sync = (mode == core::SyncMode::Durable);
    s = flushBuffer(sync);
    if (!s.ok()) return s;
    if (!segments_.empty()) segments_.back().lastCommitTxnId = txnId;
    recoveredTxnId_ = txnId;
    return Status::OK();
}

Status Wal::checkpoint(TxnId upToTxnId, core::SyncMode mode) {
    // CONTRACT: the caller (PageManager::commit) must have already made the
    // corresponding data pages + meta durable *before* invoking checkpoint
    // when SyncMode is Durable/Flush. Deleting a segment whose page images
    // have not been msync'd to the data file would leave recovery with no
    // redo source after a crash (meta already advanced, WAL gone).
    CheckpointRecord rec{upToTxnId};
    Status s = encodeCheckpoint(rec, writeBuffer_);
    if (!s.ok()) return s;
    bool sync = (mode == core::SyncMode::Durable);
    s = flushBuffer(sync);
    if (!s.ok()) return s;

    // Drop fully-covered *closed* segments. Keep the active segment and any
    // segment that may still hold post-checkpoint data. Heuristic: delete
    // segments whose recorded lastCommitTxnId <= upToTxnId and that are not
    // the last segment. Segments without a tracked commit are kept.
    while (segments_.size() > options_.minSegmentsToKeep) {
        Segment& oldest = segments_.front();
        if (oldest.lastCommitTxnId == kInvalidTxnId) break;
        if (oldest.lastCommitTxnId > upToTxnId) break;
        // Never delete the active (last) segment here.
        if (segments_.size() == 1) break;
        s = closeSegment(oldest);
        if (!s.ok()) return s;
        s = removePath(oldest.path);
        if (!s.ok()) return s;
        segments_.erase(segments_.begin());
    }
    return Status::OK();
}

Status Wal::truncateAfterRecovery() {
    Status s = flushBuffer(false);
    if (!s.ok()) return s;
    for (auto& seg : segments_) {
        closeSegment(seg);
        Status rs = removePath(seg.path);
        if (!rs.ok()) return rs;
    }
    segments_.clear();
    Segment seg;
    seg.id = 1;
    seg.path = dir_ / segmentFileName(1);
    s = openSegmentForAppend(seg, true);
    if (!s.ok()) return s;
    segments_.push_back(std::move(seg));
    writeBuffer_.clear();
    return Status::OK();
}

// ---------------------------------------------------------------------------
// Streaming recovery
// ---------------------------------------------------------------------------

Result<bool> Wal::streamFill(StreamCursor& c, std::size_t need) {
    while (c.window.size() - c.windowPos < need) {
        // Compact window
        if (c.windowPos > 0) {
            if (c.windowPos < c.window.size()) {
                std::memmove(c.window.data(), c.window.data() + c.windowPos,
                             c.window.size() - c.windowPos);
                c.window.resize(c.window.size() - c.windowPos);
            } else {
                c.window.clear();
            }
            c.windowPos = 0;
        }

        if (c.segIndex >= segments_.size()) {
            return Result<bool>(false);  // EOF
        }

        Segment& seg = segments_[c.segIndex];
        // Always open a dedicated, independent read-only handle for recovery
        // scanning rather than reusing seg.handle (the append-mode write
        // handle). On Windows, a positioned ReadFile via OVERLAPPED on a
        // handle opened WITHOUT FILE_FLAG_OVERLAPPED still updates that
        // handle's shared internal file position as a side effect; reusing
        // the write handle here would silently displace where the next
        // sequential WriteFile append lands, corrupting existing WAL bytes.
        Fd fd = kInvalidFd;
        Status openStatus = openFdRead(seg.path, fd);
        if (!openStatus.ok()) return openStatus;
        bool openedHere = true;
        if (seg.size == 0) {
            Status s = fileSizeFd(fd, seg.size);
            if (!s.ok()) {
                if (openedHere) closeFd(fd);
                return s;
            }
        }

        if (c.fileOffset >= seg.size) {
            if (openedHere) closeFd(fd);
            ++c.segIndex;
            c.fileOffset = 0;
            continue;
        }

        std::size_t chunk = options_.recoveryReadChunk;
        std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunk, seg.size - c.fileOffset));
        std::size_t old = c.window.size();
        c.window.resize(old + want);
        std::size_t got = 0;
        Status s = preadFd(fd, c.window.data() + old, want, c.fileOffset, got);
        if (openedHere) closeFd(fd);
        if (!s.ok()) return s;
        c.window.resize(old + got);
        c.fileOffset += got;
        if (got == 0) {
            ++c.segIndex;
            c.fileOffset = 0;
        }
    }
    return Result<bool>(true);
}

Result<bool> Wal::streamNextRecord(StreamCursor& c, RecordType& type, ByteBuffer& body) {
    // Need at least header
    auto filled = streamFill(c, kRecordHeaderBytes);
    if (!filled) return filled.status();
    if (!filled.value()) return Result<bool>(false);

    ByteView hdr(c.window.data() + c.windowPos, c.window.size() - c.windowPos);
    RecordType rtype;
    auto bodyR = validateRecordHeader(hdr, rtype);
    if (!bodyR) {
        // Truncated / corrupt tail — stop cleanly.
        return Result<bool>(false);
    }
    // bodyR is a view into hdr; compute length and pull full body into owned buffer.
    // validateRecordHeader returns body view; total record = header + body.size().
    ByteView bodyView = bodyR.value();
    std::size_t total = kRecordHeaderBytes + bodyView.size();
    filled = streamFill(c, total);
    if (!filled) return filled.status();
    if (!filled.value()) return Result<bool>(false);

    // Re-validate after fill (window may have compacted)
    hdr = ByteView(c.window.data() + c.windowPos, c.window.size() - c.windowPos);
    bodyR = validateRecordHeader(hdr, rtype);
    if (!bodyR) return Result<bool>(false);
    bodyView = bodyR.value();
    body.assign(bodyView.begin(), bodyView.end());
    type = rtype;
    c.windowPos += kRecordHeaderBytes + body.size();
    return Result<bool>(true);
}

Result<RecoveryResult> Wal::recover(core::PageManager& pm) {
    RecoveryResult out;
    out.segmentsScanned = static_cast<std::uint32_t>(segments_.size());

    // Ensure sizes known for closed segments.
    for (auto& seg : segments_) {
        if (seg.size != 0) continue;
        Fd fd = kInvalidFd;
        Status s = openFdRead(seg.path, fd);
        if (!s.ok()) return s;
        s = fileSizeFd(fd, seg.size);
        closeFd(fd);
        if (!s.ok()) return s;
    }

    // ---- Pass 1: find last complete Commit (stream) ----
    struct CommitPos {
        CommitRecord rec;
        std::size_t segIndex = 0;
        std::uint64_t endOffset = 0;  // file offset in that segment after the record
        std::uint64_t globalBytes = 0;
    };
    std::optional<CommitPos> lastCommit;
    TxnId lastCheckpoint = kInvalidTxnId;
    std::uint64_t globalBytes = 0;

    StreamCursor c;
    for (;;) {
        RecordType type;
        ByteBuffer body;
        auto next = streamNextRecord(c, type, body);
        if (!next) return next.status();
        if (!next.value()) break;

        globalBytes += kRecordHeaderBytes + body.size();
        out.validLogBytes = globalBytes;

        switch (type) {
            case RecordType::Commit: {
                CommitRecord rec;
                Status s = decodeCommit(ByteView(body.data(), body.size()), rec);
                if (!s.ok()) return s;
                lastCommit = CommitPos{rec, c.segIndex, c.fileOffset, globalBytes};
                if (c.segIndex < segments_.size()) {
                    // Track for checkpoint GC
                    segments_[c.segIndex].lastCommitTxnId = rec.txnId;
                }
                break;
            }
            case RecordType::Checkpoint: {
                CheckpointRecord rec;
                Status s = decodeCheckpoint(ByteView(body.data(), body.size()), rec);
                if (!s.ok()) return s;
                lastCheckpoint = rec.upToTxnId;
                break;
            }
            default:
                break;
        }
    }
    out.lastCheckpointTxnId = lastCheckpoint;

    if (!lastCommit.has_value()) {
        return Result<RecoveryResult>(out);
    }

    const CommitRecord& commit = lastCommit->rec;
    out.lastCommitTxnId = commit.txnId;
    out.lastMasterRoot = commit.masterRootPageId;
    out.lastFreeListRoot = commit.freeListRootPageId;
    out.lastWalSequence = commit.walSequence;
    out.lastPageSize = commit.pageSize;
    const bool metaNeedsInstall = commit.txnId > pm.lastCommittedTxnId();

    // ---- Pass 2: redo through last Commit (stream again) ----
    pm.setRecoveryMode(true);
    StreamCursor c2;
    std::uint64_t applied = 0;
    const std::uint64_t limit = lastCommit->globalBytes;

    while (applied < limit) {
        RecordType type;
        ByteBuffer body;
        auto next = streamNextRecord(c2, type, body);
        if (!next) {
            pm.setRecoveryMode(false);
            return next.status();
        }
        if (!next.value()) break;
        applied += kRecordHeaderBytes + body.size();
        if (applied > limit) break;

        switch (type) {
            case RecordType::PageImage:
            case RecordType::PageImageV2: {
                PageImageRecord rec;
                Status s = decodePageImage(ByteView(body.data(), body.size()), type, pm.pageSize(), rec);
                if (!s.ok()) {
                    pm.setRecoveryMode(false);
                    return s;
                }
                auto page = pm.writePageAt(rec.pageId, commit.txnId, rec.type);
                if (!page) {
                    pm.setRecoveryMode(false);
                    return page.status();
                }
                core::PageHeader* header = page.value();
                header->pageId = rec.pageId;
                header->txnId = commit.txnId;
                header->type = rec.type;
                header->payloadSize = rec.payloadSize;
                header->itemCount = rec.itemCount;
                // writePageAt reset these. Restoring overflowNext is what
                // keeps a multi-page overflow chain linked after replay; a
                // legacy PageImage record has neither, so it decodes to the
                // old (chain-cutting) defaults.
                header->flags = rec.flags;
                header->overflowNext = rec.overflowNext;
                const std::size_t copyLen =
                    std::min(rec.payload.size(), core::payloadCapacity(pm.pageSize()));
                std::memcpy(core::PageManager::payload(header), rec.payload.data(), copyLen);
                pm.finalizePage(header, rec.payloadSize, rec.itemCount);
                break;
            }
            case RecordType::FreeListUpdate: {
                FreeListUpdateRecord rec;
                Status s = decodeFreeListUpdate(ByteView(body.data(), body.size()), rec);
                if (!s.ok()) {
                    pm.setRecoveryMode(false);
                    return s;
                }
                pm.freeList().retire(rec.retiredPages, rec.retiringTxnId);
                break;
            }
            default:
                break;
        }
    }
    pm.setRecoveryMode(false);

    Status fs = pm.flushAll(core::SyncMode::Durable);
    if (!fs.ok()) return fs;

    if (metaNeedsInstall) {
        Status is = pm.installRecoveredCommit(commit.txnId, commit.masterRootPageId,
                                              commit.freeListRootPageId, commit.walSequence,
                                              core::SyncMode::Durable);
        if (!is.ok()) return is;
        out.appliedCommit = true;
    }

    recoveredTxnId_ = commit.txnId;
    return Result<RecoveryResult>(out);
}

Result<TxnId> Wal::replayInto(core::PageManager& pm) {
    auto r = recover(pm);
    if (!r) return r.status();
    return Result<TxnId>(r.value().lastCommitTxnId);
}

}  // namespace annpocketdb::wal
