#include "annpocketdb/core/checksum_sidecar.hpp"
#include "annpocketdb/util/path.hpp"

#include <cstring>

namespace annpocketdb::core {

Status ChecksumSidecar::open(const std::filesystem::path& dataPath, bool readOnly,
                             std::uint64_t maxDatabaseSize, std::uint32_t pageSize) {
    std::filesystem::path sidecarPath = util::withSuffix(dataPath, ".sha256");
    std::uint64_t maxRecords = (maxDatabaseSize + pageSize - 1) / pageSize;
    std::uint64_t maxSidecarBytes = maxRecords * kRecordSize;

    std::error_code ec;
    bool existed = std::filesystem::exists(sidecarPath, ec) && !ec &&
                   std::filesystem::file_size(sidecarPath, ec) > 0;
    std::uint64_t initialSize = existed ? 0 : kRecordSize * 1024;

    auto mode = readOnly ? platform::OpenMode::ReadOnly : platform::OpenMode::ReadWrite;
    auto r = platform::MappedFile::open(sidecarPath, mode, initialSize, maxSidecarBytes);
    if (!r) return r.status();
    mapped_ = r.moveValue();
    return Status::OK();
}

Status ChecksumSidecar::close() {
    if (!mapped_.isOpen()) return Status::OK();
    return mapped_.close();
}

Status ChecksumSidecar::ensureCovers(PageId id) {
    std::uint64_t required = (static_cast<std::uint64_t>(id) + 1) * kRecordSize;
    if (required <= mapped_.fileSize()) return Status::OK();
    constexpr std::uint64_t kGrowthRecords = 4096;
    std::uint64_t chunk = kGrowthRecords * kRecordSize;
    std::uint64_t rounded = ((required + chunk - 1) / chunk) * chunk;
    Status rs = mapped_.resize(rounded);
    if (!rs.ok()) return mapped_.resize(required);
    return Status::OK();
}

void ChecksumSidecar::read(PageId id, std::array<std::uint8_t, kRecordSize>& digest) const {
    std::uint64_t offset = static_cast<std::uint64_t>(id) * kRecordSize;
    if (!mapped_.isOpen() || offset + kRecordSize > mapped_.fileSize()) {
        digest.fill(0);
        return;
    }
    std::memcpy(digest.data(), mapped_.data() + offset, kRecordSize);
}

Status ChecksumSidecar::write(PageId id, const std::array<std::uint8_t, kRecordSize>& digest) {
    Status rs = ensureCovers(id);
    if (!rs.ok()) return rs;
    std::memcpy(mapped_.data() + static_cast<std::uint64_t>(id) * kRecordSize, digest.data(), kRecordSize);
    return Status::OK();
}

Status ChecksumSidecar::flush(bool sync) {
    if (!mapped_.isOpen()) return Status::OK();
    return mapped_.flush(0, mapped_.fileSize(), sync);
}

}  // namespace annpocketdb::core
