#include "annpocketdb/core/free_list.hpp"

namespace annpocketdb::core {

PageId FreeList::popReusablePage() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reusable_.empty()) return kInvalidPageId;
    return reusable_.popBack();
}
void FreeList::retire(PageId pageId, TxnId retiringTxnId) {
    std::lock_guard<std::mutex> lock(mutex_);
    retired_.emplace(retiringTxnId, pageId);
}
void FreeList::retire(const std::vector<PageId>& pageIds, TxnId retiringTxnId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (PageId id : pageIds) retired_.emplace(retiringTxnId, id);
}
void FreeList::unretire(PageId pageId, TxnId retiringTxnId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto range = retired_.equal_range(retiringTxnId);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == pageId) {
            retired_.erase(it);
            return;
        }
    }
}
void FreeList::reclaim(TxnId oldestActiveReaderTxnId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto end = retired_.lower_bound(oldestActiveReaderTxnId);
    for (auto it = retired_.begin(); it != end; ++it) reusable_.pushBack(it->second);
    retired_.erase(retired_.begin(), end);
}
std::size_t FreeList::reusableCount() const { std::lock_guard<std::mutex> lock(mutex_); return reusable_.size(); }
std::size_t FreeList::retiredCount() const { std::lock_guard<std::mutex> lock(mutex_); return retired_.size(); }

}  // namespace annpocketdb::core
