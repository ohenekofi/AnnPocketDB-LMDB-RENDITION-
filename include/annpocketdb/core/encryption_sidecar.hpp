#pragma once
//
// AnnPocketDb :: core/encryption_sidecar.hpp
//
// Per-page encryption metadata (nonce counter + AEAD tag), one fixed-size
// record per PageId, stored in a small file beside the main data file
// (<path>.enc). Exists so PageManager never has to shrink the main file's
// page payload capacity to make room for a MAC -- the ciphertext body still
// occupies exactly pageSize bytes in the main file; this file just carries
// the extra bytes needed to verify/decrypt it, at the same PageId-indexed
// offset scheme (id * kRecordSize) the main file uses (id * pageSize).
//
// Durability mirrors the main data file: written eagerly (in memory, via
// the mmap) whenever a page is finalized, and explicitly flushed alongside
// the main file at the same commit-time barrier that gates WAL checkpoint
// pruning (see PageManager::flushAll/commit) -- so once a checkpoint has
// pruned old WAL segments, this file is guaranteed exactly as durable as
// the page data it describes. Before that point, WAL redo naturally
// rebuilds it: WAL page-image redo already restores a page's plaintext via
// writePageAt + finalizePage, and finalizePage (when encryption is enabled)
// re-derives and rewrites this file's entry as an ordinary side effect --
// no WAL wire-format change needed.
//
// Meta pages (id 0/1) never have an entry here -- they are never encrypted
// (see PageManager: the engine must be able to read pageSize/format before
// any key material exists).
//
#include <array>
#include <cstdint>
#include <filesystem>

#include "annpocketdb/platform/mapped_file.hpp"
#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

namespace annpocketdb::core {

class EncryptionSidecar {
public:
    static constexpr std::size_t kRecordSize = 24;  // 8-byte counter + 16-byte MAC

    EncryptionSidecar() = default;

    // Opens (creating if necessary) `<dataPath>.enc`. `maxDatabaseSize`/
    // `pageSize` size this file's own virtual reservation proportionally
    // (one kRecordSize-byte slot per pageSize-byte slot in the main file).
    Status open(const std::filesystem::path& dataPath, bool readOnly,
                std::uint64_t maxDatabaseSize, std::uint32_t pageSize);
    Status close();
    bool isOpen() const { return mapped_.isOpen(); }

    // Reads the current nonce counter + MAC for `id`. A page never yet
    // written to the sidecar reads back as counter=0, mac=all-zero (a
    // fresh page's first encryptPage call always bumps the counter to 1
    // before use, so 0 is never a valid in-use counter value).
    void read(PageId id, std::uint64_t& counter, std::array<std::uint8_t, 16>& mac) const;
    // Writes a new counter + MAC for `id`, growing the file if needed.
    Status write(PageId id, std::uint64_t counter, const std::array<std::uint8_t, 16>& mac);

    // Flushes the sidecar's dirty range to stable storage, mirroring
    // PageManager::flushAll's call on the main file. `sync` mirrors
    // MappedFile::flush's own parameter (true => fsync/MS_SYNC-equivalent).
    Status flush(bool sync);

private:
    Status ensureCovers(PageId id);
    platform::MappedFile mapped_;
};

}  // namespace annpocketdb::core
