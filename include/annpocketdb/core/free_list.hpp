#pragma once
// AnnPocketDb :: core/free_list.hpp
// Two-stage MVCC-safe page reclamation:
//  1. ReclaimableList (reusable_): page ids durably known unreachable by any
//     reader; persisted to disk as a chain of Free-type pages.
//  2. RetiredSet (retired_): page ids freed by the writer's most recent
//     commit, which older still-open readers may still be traversing.
//     These become reclaimable only once no active reader's snapshot is
//     old enough to still see them.
//
// Operational note — process-lifetime retired set:
//   Only stage-1 (reusable_) pages are persisted across restarts. Stage-2
//   (retired_) is intentionally process-local: a crash before those pages
//   are promoted simply means their space is not reused until a later
//   backup/restore or compaction cycle. That is safe (no reader can still
//   hold a pointer into a dead process) but under frequent crashes combined
//   with long-lived readers, free space can accumulate. Operators should
//   plan periodic online backup/compact (see the backup module roadmap) to
//   reclaim that space. This is not a correctness issue.
#include <cstdint>
#include <map>
#include <vector>
#include <mutex>
#include <algorithm>
#include "annpocketdb/util/types.hpp"
#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/bounded_walk.hpp"
#include "annpocketdb/util/page_id_list.hpp"

namespace annpocketdb::core {

class FreeList {
public:
    PageId popReusablePage();
    void retire(PageId pageId, TxnId retiringTxnId);
    void retire(const std::vector<PageId>& pageIds, TxnId retiringTxnId);
    // Undo a single retire (nested-txn abort).
    void unretire(PageId pageId, TxnId retiringTxnId);
    void reclaim(TxnId oldestActiveReaderTxnId);
    std::size_t reusableCount() const;
    std::size_t retiredCount() const;

    // Serializes the reusable pool as a chain of Free-type pages. Kept
    // intentionally simple for the kernel phase: pending retired pages
    // (stage 2) are process-lifetime only and are correctly NOT persisted
    // -- a crash before promotion just means that space is not reused until
    // a later recovery/backup cycle, never a correctness problem. Only
    // fully-reclaimed reusable pages (stage 1) are persisted.
    template <typename PageWriter, typename PageAllocator>
    Result<PageId> persist(PageId headPageId, PageWriter&& writePage, PageAllocator&& allocatePageForFreelist);

    // Loads the reusable pool from a Free-page chain. `readPage` is expected to
    // obtain pages via PageManager::getPage() so payloadSize is CRC-checked (and,
    // under ANNPOCKETDB_PARANOID, structurally validated) before the caller divides
    // payloadSize by sizeof(PageId). Stage-2 retired pages remain process-local.
    template <typename PageReader>
    Status load(PageId headPageId, PageReader&& readPage);

private:
    mutable std::mutex mutex_;
    util::PageIdList reusable_;
    std::multimap<TxnId, PageId> retired_;
};

template <typename PageWriter, typename PageAllocator>
Result<PageId> FreeList::persist(PageId /*headPageId*/, PageWriter&& writePage, PageAllocator&& allocatePageForFreelist) {
    // IMPORTANT: extract the reusable list and release mutex_ *before*
    // invoking the caller-supplied callbacks. allocatePageForFreelist calls
    // back into PageManager::allocatePage(), which itself calls
    // FreeList::popReusablePage() -- holding mutex_ across that callback
    // would self-deadlock the single calling thread against its own lock.
    std::vector<PageId> local;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reusable_.empty()) return Result<PageId>(kInvalidPageId);
        local = reusable_.releaseAll();
    }

    constexpr std::size_t kIdsPerChunk = 400;
    std::size_t total = local.size();
    std::size_t offset = 0;
    std::vector<std::pair<PageId, std::vector<PageId>>> chunks;
    while (offset < total) {
        std::size_t take = std::min(kIdsPerChunk, total - offset);
        std::vector<PageId> chunk(local.begin() + static_cast<long>(offset),
                                   local.begin() + static_cast<long>(offset + take));
        chunks.emplace_back(kInvalidPageId, std::move(chunk));
        offset += take;
    }
    PageId nextLink = kInvalidPageId;
    for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) {
        PageId pid = allocatePageForFreelist();
        it->first = pid;
        writePage(pid, it->second, nextLink);
        nextLink = pid;
    }
    return Result<PageId>(nextLink);
}

template <typename PageReader>
Status FreeList::load(PageId headPageId, PageReader&& readPage) {
    std::lock_guard<std::mutex> lock(mutex_);
    reusable_.clear();
    PageId cur = headPageId;
    // Cap hops so a corrupted overflowNext cycle cannot hang Environment::open.
    BoundedWalk walk(kMaxChainStepsFallback);
    while (cur != kInvalidPageId) {
        Status bs = walk.step("free-list chain");
        if (!bs.ok()) return bs;
        std::vector<PageId> ids;
        PageId next = kInvalidPageId;
        Status s = readPage(cur, ids, next);
        if (!s.ok()) return s;
        reusable_.appendRange(ids);
        cur = next;
    }
    return Status::OK();
}

}  // namespace annpocketdb::core
