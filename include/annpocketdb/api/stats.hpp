#pragma once
//
// AnnPocketDb :: api/stats.hpp
//
// LMDB-style environment / database statistics and reader introspection.
//
#include <cstdint>
#include <vector>

#include "annpocketdb/util/types.hpp"

namespace annpocketdb::api {

// Per-tree statistics (mdb_stat analogue). Filled by walking one B+tree root.
struct DbStat {
    std::uint32_t pageSize = 0;
    std::uint32_t depth = 0;           // root depth (1 = single leaf root)
    std::uint64_t branchPages = 0;
    std::uint64_t leafPages = 0;
    std::uint64_t overflowPages = 0;
    std::uint64_t entries = 0;         // leaf entry count (keys, or key+value under dups)
    std::uint64_t freePages = 0;       // reusable + retired (process-visible)
    std::uint64_t pagesUsed = 0;       // branch + leaf + overflow observed in walk
};

// Environment-level information (mdb_env_info analogue).
struct EnvInfo {
    std::uint64_t mapSize = 0;
    std::uint64_t lastPageId = 0;      // nextFreePageId - 1 (allocation watermark)
    std::uint64_t lastTxnId = 0;
    std::uint32_t maxReaders = 0;
    std::uint32_t numReaders = 0;
    std::uint32_t pageSize = 0;
    std::uint64_t fileSize = 0;
    bool multiProcess = false;
};

// One occupied reader slot (mdb_reader_list entry).
struct ReaderInfo {
    std::uint32_t slot = 0;
    std::uint64_t pid = 0;             // 0 in single-process mode
    TxnId snapshotTxnId = kInvalidTxnId;
};

}  // namespace annpocketdb::api
