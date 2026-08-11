#pragma once
//
// AnnPocketDb :: util/page_id_list.hpp
//
// Sorted PageId list utility -- this project's equivalent of LMDB's midl.c.
// A thin wrapper around std::vector<PageId> adding binary-search insert/
// contains/erase and sorted-merge helpers, plus the plain order-preserving
// push/pop operations core::FreeList's reusable pool already relied on --
// consolidating that ad hoc vector manipulation onto one named, testable
// type without changing its existing LIFO reuse order (see free_list.cpp).
//
// Callers choose the discipline: pushBack/popBack/appendRange preserve
// insertion order and make no sortedness claim. insertSorted/contains/
// eraseSorted/mergeSorted require the list to already be sorted ascending
// (guaranteed after any sequence of insertSorted-only calls, or after an
// explicit sort()) -- mixing the two families without an intervening sort()
// is the caller's responsibility, exactly like std::vector's own contract
// with std::lower_bound/std::binary_search.
//
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <vector>

#include "annpocketdb/util/types.hpp"

namespace annpocketdb::util {

class PageIdList {
public:
    PageIdList() = default;

    bool empty() const { return ids_.empty(); }
    std::size_t size() const { return ids_.size(); }
    void clear() { ids_.clear(); }
    void reserve(std::size_t n) { ids_.reserve(n); }

    // ---- Order-preserving stack/bulk-append use ----
    void pushBack(PageId id) { ids_.push_back(id); }
    PageId popBack() {
        PageId id = ids_.back();
        ids_.pop_back();
        return id;
    }
    // Appends `others` in their existing relative order; does not sort.
    void appendRange(const std::vector<PageId>& others) {
        ids_.insert(ids_.end(), others.begin(), others.end());
    }
    // Moves the whole list out (for a caller that wants to take ownership,
    // e.g. to hand off to a chunked writer) and resets this list to empty.
    std::vector<PageId> releaseAll() {
        std::vector<PageId> out = std::move(ids_);
        ids_.clear();
        return out;
    }

    // ---- Sorted-array (midl-style) use ----
    // Binary-search insert maintaining ascending order. O(log n) search +
    // O(n) shift. Requires the list to already be sorted ascending.
    void insertSorted(PageId id) {
        auto it = std::lower_bound(ids_.begin(), ids_.end(), id);
        ids_.insert(it, id);
    }
    // O(log n). Requires the list to already be sorted ascending.
    bool contains(PageId id) const { return std::binary_search(ids_.begin(), ids_.end(), id); }
    // O(log n) search + O(n) shift. Returns true if `id` was present and removed.
    // Requires the list to already be sorted ascending.
    bool eraseSorted(PageId id) {
        auto it = std::lower_bound(ids_.begin(), ids_.end(), id);
        if (it == ids_.end() || *it != id) return false;
        ids_.erase(it);
        return true;
    }
    // Merges `other` into this list, preserving ascending order. Both lists
    // must already be sorted ascending. Duplicate ids present in both are
    // both kept (set semantics, if wanted, is the caller's job beforehand).
    void mergeSorted(const PageIdList& other) {
        std::vector<PageId> out;
        out.reserve(ids_.size() + other.ids_.size());
        std::merge(ids_.begin(), ids_.end(), other.ids_.begin(), other.ids_.end(),
                   std::back_inserter(out));
        ids_ = std::move(out);
    }
    void sort() { std::sort(ids_.begin(), ids_.end()); }
    bool isSorted() const { return std::is_sorted(ids_.begin(), ids_.end()); }

    // ---- Raw access ----
    const std::vector<PageId>& raw() const { return ids_; }
    PageId operator[](std::size_t i) const { return ids_[i]; }
    std::vector<PageId>::const_iterator begin() const { return ids_.begin(); }
    std::vector<PageId>::const_iterator end() const { return ids_.end(); }

private:
    std::vector<PageId> ids_;
};

}  // namespace annpocketdb::util
