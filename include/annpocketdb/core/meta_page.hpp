#pragma once
// AnnPocketDb :: core/meta_page.hpp
// Root of trust for the database: committed txn id, master btree root,
// free-list head, format/feature versioning. Two meta pages (id 0 and 1)
// are written alternately so a crash mid-write to one leaves the other
// (previously committed) meta page intact -- commit is then just "pick the
// valid meta page with the higher committed txn id".
#include <cstdint>
#include <array>
#include "annpocketdb/util/types.hpp"

namespace annpocketdb::core {

constexpr std::uint32_t kMagic = 0x41504442u;  // "APDB"
constexpr std::uint32_t kFormatVersion = 1;

enum FeatureFlags : std::uint32_t {
    kFeatureNone = 0,
    kFeatureWal = 1 << 0,
    kFeatureCrc32c = 1 << 1,
    kFeatureCompression = 1 << 2,
    kFeatureEncryption = 1 << 3,
    kFeatureBloom = 1 << 4,
    kFeatureBlob = 1 << 5,
    kFeatureDocument = 1 << 6,
};

// Two-phase commit marker (WriteTransaction::prepare/commit/abort; see
// PageManager::loadExistingFile's slot-selection rule). A slot durably
// written by prepare() but never finalized (process crashed before the
// coordinator's decision) must never be picked up as authoritative just
// because its txnId is higher -- Committed always outranks Prepared
// regardless of txnId; only same-commitState slots are compared by txnId.
enum class MetaCommitState : std::uint8_t {
    Committed = 0,  // default: every pre-existing/normal meta page is unaffected
    Prepared = 1,
    RolledBack = 2,  // explicitly invalidated by abort-after-prepare / Environment::rollbackPrepared
};

#pragma pack(push, 1)
struct MetaPageBody {
    std::uint32_t magic = kMagic;
    std::uint32_t formatVersion = kFormatVersion;
    std::uint32_t pageSize = 0;
    std::uint32_t featureFlags = kFeatureNone;
    std::array<std::uint8_t, 16> uuid{};
    TxnId lastCommittedTxnId = 0;
    PageId masterRootPageId = kInvalidPageId;
    PageId freeListRootPageId = kInvalidPageId;
    PageId nextFreePageId = 2;
    SequenceNumber walSequence = 0;
    std::uint64_t creationEngineVersion = 0;
    MetaCommitState commitState = MetaCommitState::Committed;
    std::uint8_t reservedBytes[7] = {0, 0, 0, 0, 0, 0, 0};
    std::uint64_t reserved[3] = {0, 0, 0};
};
#pragma pack(pop)

static_assert(sizeof(MetaPageBody) <= 4096 - 40, "MetaPageBody must fit within a minimum-size page payload");

}  // namespace annpocketdb::core
