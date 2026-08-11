#include "annpocketdb/core/encryption_sidecar.hpp"

#include <algorithm>
#include <cstring>

namespace annpocketdb::core {

Status EncryptionSidecar::open(const std::filesystem::path& dataPath, bool readOnly,
                               std::uint64_t maxDatabaseSize, std::uint32_t pageSize) {
    std::filesystem::path sidecarPath = dataPath.string() + ".enc";

    // Proportional virtual reservation: one kRecordSize-byte slot per
    // pageSize-byte page slot the main file could ever reach.
    std::uint64_t maxRecords = (maxDatabaseSize + pageSize - 1) / pageSize;
    std::uint64_t maxSidecarBytes = maxRecords * kRecordSize;

    std::error_code ec;
    bool existed = std::filesystem::exists(sidecarPath, ec) && !ec &&
                   std::filesystem::file_size(sidecarPath, ec) > 0;
    // Headroom for the first pages without an immediate resize; page ids 0/1
    // (meta) never get a sidecar entry, but reserving from id 0 keeps the
    // id*kRecordSize offset scheme trivial (a few wasted bytes, not worth
    // the complexity of an id-2-based offset).
    std::uint64_t initialSize = existed ? 0 : kRecordSize * 1024;

    auto mode = readOnly ? platform::OpenMode::ReadOnly : platform::OpenMode::ReadWrite;
    auto r = platform::MappedFile::open(sidecarPath, mode, initialSize, maxSidecarBytes);
    if (!r) return r.status();
    mapped_ = r.moveValue();
    return Status::OK();
}

Status EncryptionSidecar::close() {
    if (!mapped_.isOpen()) return Status::OK();
    return mapped_.close();
}

Status EncryptionSidecar::ensureCovers(PageId id) {
    std::uint64_t required = (static_cast<std::uint64_t>(id) + 1) * kRecordSize;
    if (required <= mapped_.fileSize()) return Status::OK();
    constexpr std::uint64_t kGrowthRecords = 4096;
    std::uint64_t chunk = kGrowthRecords * kRecordSize;
    std::uint64_t rounded = ((required + chunk - 1) / chunk) * chunk;
    Status rs = mapped_.resize(rounded);
    if (!rs.ok()) return mapped_.resize(required);
    return Status::OK();
}

void EncryptionSidecar::read(PageId id, std::uint64_t& counter, std::array<std::uint8_t, 16>& mac) const {
    std::uint64_t offset = static_cast<std::uint64_t>(id) * kRecordSize;
    if (!mapped_.isOpen() || offset + kRecordSize > mapped_.fileSize()) {
        counter = 0;
        mac.fill(0);
        return;
    }
    const std::byte* p = mapped_.data() + offset;
    std::memcpy(&counter, p, 8);
    std::memcpy(mac.data(), p + 8, 16);
}

Status EncryptionSidecar::write(PageId id, std::uint64_t counter, const std::array<std::uint8_t, 16>& mac) {
    Status rs = ensureCovers(id);
    if (!rs.ok()) return rs;
    std::byte* p = mapped_.data() + static_cast<std::uint64_t>(id) * kRecordSize;
    std::memcpy(p, &counter, 8);
    std::memcpy(p + 8, mac.data(), 16);
    return Status::OK();
}

Status EncryptionSidecar::flush(bool sync) {
    if (!mapped_.isOpen()) return Status::OK();
    return mapped_.flush(0, mapped_.fileSize(), sync);
}

}  // namespace annpocketdb::core
