#pragma once
//
// AnnPocketDb :: core/checksum_sidecar.hpp
//
// Optional SHA-256 page checksum bolt-on (LMDB-fork-inspired): an
// *additional* integrity check layered on top of the always-on CRC32C, not
// a replacement -- CRC32C is fast but has real collision weaknesses SHA-256
// doesn't. One 32-byte digest per PageId, stored the same way encryption's
// nonce/MAC pair is (see core/encryption_sidecar.hpp): a small sidecar file
// (<path>.sha256) rather than shrinking the B+tree's page payload capacity.
//
// Durability/redo story is identical to EncryptionSidecar's: written
// whenever a page is finalized, flushed alongside the main file at the same
// commit-time barrier that gates WAL checkpoint pruning, and naturally
// rebuilt by WAL redo (which restores a page via writePageAt + a second
// finalizePage call) with no WAL format change.
//
#include <array>
#include <cstdint>
#include <filesystem>

#include "annpocketdb/platform/mapped_file.hpp"
#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

namespace annpocketdb::core {

class ChecksumSidecar {
public:
    static constexpr std::size_t kRecordSize = 32;  // SHA-256 digest

    ChecksumSidecar() = default;

    Status open(const std::filesystem::path& dataPath, bool readOnly,
                std::uint64_t maxDatabaseSize, std::uint32_t pageSize);
    Status close();
    bool isOpen() const { return mapped_.isOpen(); }

    // A page never yet written reads back as all-zero -- callers should
    // only compare once they know the page has actually been through
    // finalizePage at least once (true for every page returned by getPage).
    void read(PageId id, std::array<std::uint8_t, kRecordSize>& digest) const;
    Status write(PageId id, const std::array<std::uint8_t, kRecordSize>& digest);

    Status flush(bool sync);

private:
    Status ensureCovers(PageId id);
    platform::MappedFile mapped_;
};

}  // namespace annpocketdb::core
