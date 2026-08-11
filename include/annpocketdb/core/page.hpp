#pragma once
//
// AnnPocketDb :: core/page.hpp
//
// On-disk page header and type tags shared by every layer that touches pages.
//
#include <cstdint>
#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

namespace annpocketdb::core {

constexpr std::uint32_t kMinPageSize = 512;
constexpr std::uint32_t kDefaultPageSize = 4096;
constexpr std::uint32_t kMaxPageSize = 65536;

enum class PageType : std::uint16_t {
    Meta = 1,
    Leaf = 2,
    Branch = 3,
    Overflow = 4,
    Free = 5,
};

#pragma pack(push, 1)
struct PageHeader {
    PageId pageId = kInvalidPageId;
    TxnId txnId = kInvalidTxnId;
    PageType type = PageType::Leaf;
    std::uint16_t flags = 0;
    std::uint16_t itemCount = 0;
    std::uint32_t payloadSize = 0;
    PageId overflowNext = kInvalidPageId;
    std::uint32_t crc32c = 0;
};
#pragma pack(pop)

inline std::size_t payloadCapacity(std::uint32_t pageSize) {
    return static_cast<std::size_t>(pageSize) - sizeof(PageHeader);
}

// When ANNPOCKETDB_PARANOID is defined, PageManager::getPage() runs this after
// the CRC check. Validates payloadSize / itemCount and, for Leaf/Branch pages,
// every slot offset+length and decoded keyLen/valueLen against the page
// capacity. Off by default so the hot path stays LMDB-parity fast.
Status validatePage(const PageHeader* header, std::uint32_t pageSize);

}  // namespace annpocketdb::core
