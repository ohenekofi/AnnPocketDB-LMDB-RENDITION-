#pragma once
//
// AnnPocketDb :: core/reader_table.hpp
//
// Tracks the MVCC snapshot txnId of every currently-open ReadTransaction so
// the writer can compute, at commit time, the oldest txnId any reader could
// still be observing (see FreeList's two-stage reclaim: a page retired by
// txn T is only safe to reuse once every active reader's snapshot is > T).
//
// Reconstructed from usage, since this header was missing from the review
// dump. Call sites that constrain the API:
// - txn/transaction.cpp:
// ReadTransaction holds a `core::ReaderHandle handle_`; destructor and
// move-assignment call `pm_->unregisterReader(handle_)` guarded by
// `handle_ != core::kInvalidReaderHandle`.
// - core/page_manager.hpp/.cpp:
// `ReaderHandle registerReader(TxnId snapshotTxnId)`
// `void unregisterReader(ReaderHandle handle)`
// `std::size_t activeReaderCount() const`
// `freeList_.reclaim(readerTable_.oldestActiveSnapshot(newTxnId))`
// -- called with the *new* commit's txnId as the "nothing to protect"
// fallback when no readers are active, so that must be this
// class's fallback-argument contract too.
//
// Design notes (this is the "improve it" part, not just a reconstruction):
//
// 1. Lock-free hot path. LMDB itself uses a fixed array of reader slots
// scanned/CAS'd without a global lock, specifically because reader
// register/unregister happens on every single ReadTransaction open/close
// and a mutex there would make read-heavy workloads contend with each
// other for no reason (there's no logical need to serialize two readers
// against each other -- only the writer's oldestActiveSnapshot() scan
// needs to see them). This mirrors that: registerReader/unregisterReader
// on the common path never take a lock.
//
// 2. Graceful overflow instead of a hard failure. LMDB has a compile-time
// fixed reader-slot count and returns MDB_READERS_FULL when it's
// exhausted -- a well-known operational gotcha for anyone who scales a
// reader thread pool past the default (126 slots) without knowing to
// raise it. Here, once the lock-free inline table is full we fall back
// to a mutex-guarded overflow map instead of failing the transaction.
// The overflow path is intentionally the "cold" path: it should only
// ever be exercised under a burst larger than kInlineCapacity
// concurrent readers, at which point a lock is a fine price to pay for
// "the transaction still opens" instead of "the transaction is refused."
//
// 3. Cache-line padding on the inline slots. Multiple reader threads
// hammering registerReader/unregisterReader on physically adjacent
// atomics would false-share cache lines under contention; each slot is
// padded to 64 bytes so independent reader threads never bounce the
// same cache line off each other.
//
// 4. TxnId 0 (kInvalidTxnId) is reserved and never a legitimate snapshot
// (the first commit on a fresh file is txnId 1; WriteTransaction always
// computes txnId_ = snap.txnId + 1), so it doubles safely as the "slot
// empty" sentinel with no extra state needed per slot.
//
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include "annpocketdb/util/types.hpp"
namespace annpocketdb::core {
using ReaderHandle = std::uint64_t;
constexpr ReaderHandle kInvalidReaderHandle = static_cast<ReaderHandle>(-1);
class ReaderTable {
public:
    // Number of concurrent readers served without ever taking a lock.
    // 256 slots * 64-byte padding = 16 KiB fixed overhead per PageManager --
    // trivial, and generous relative to LMDB's default of 126. Bursts past
    // this fall back to the overflow map rather than failing.
    static constexpr std::size_t kInlineCapacity = 256;
    ReaderTable() {
        for (auto& slot : slots_) slot.txnId.store(kEmptySlot, std::memory_order_relaxed);
    }
    ReaderTable(const ReaderTable&) = delete;
    ReaderTable& operator=(const ReaderTable&) = delete;
    // Registers a new reader snapshotted at `snapshotTxnId`. Never fails:
    // if the inline table is full it degrades to the overflow path rather
    // than returning kInvalidReaderHandle, so callers do not need to treat
    // "too many concurrent readers" as an error case they must plumb
    // through Environment::beginRead(). (kInvalidReaderHandle is still part
    // of the public contract for symmetry with unregisterReader's no-op
    // guard and for any future caller that wants to represent "no reader
    // registered yet" before a handle is assigned.)
    ReaderHandle registerReader(TxnId snapshotTxnId) {
        for (std::size_t i = 0; i < kInlineCapacity; ++i) {
            TxnId expected = kEmptySlot;
            if (slots_[i].txnId.compare_exchange_strong(
                    expected, snapshotTxnId, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return static_cast<ReaderHandle>(i);
            }
        }
        std::lock_guard<std::mutex> lock(overflowMutex_);
        ReaderHandle handle = kInlineCapacity + nextOverflowId_++;
        overflow_.emplace(handle, snapshotTxnId);
        return handle;
    }
    void unregisterReader(ReaderHandle handle) {
        if (handle == kInvalidReaderHandle) return;
        if (handle < kInlineCapacity) {
            slots_[handle].txnId.store(kEmptySlot, std::memory_order_release);
            return;
        }
        std::lock_guard<std::mutex> lock(overflowMutex_);
        overflow_.erase(handle);
    }
    // Oldest snapshot txnId any currently-registered reader holds, or
    // `fallbackIfNoneActive` when there are none. Callers (PageManager::commit)
    // pass the just-committed txnId as the fallback so FreeList::reclaim
    // treats "no readers" as "everything up to this commit is safe."
    TxnId oldestActiveSnapshot(TxnId fallbackIfNoneActive) const {
        bool any = false;
        TxnId oldest = kInvalidTxnId;
        for (std::size_t i = 0; i < kInlineCapacity; ++i) {
            TxnId v = slots_[i].txnId.load(std::memory_order_acquire);
            if (v == kEmptySlot) continue;
            if (!any || v < oldest) oldest = v;
            any = true;
        }
        {
            std::lock_guard<std::mutex> lock(overflowMutex_);
            for (const auto& [handle, v] : overflow_) {
                (void)handle;
                if (!any || v < oldest) oldest = v;
                any = true;
            }
        }
        return any ? oldest : fallbackIfNoneActive;
    }
    struct SlotInfo {
        ReaderHandle handle = kInvalidReaderHandle;
        TxnId snapshotTxnId = kInvalidTxnId;
    };
    std::vector<SlotInfo> listReaders() const {
        std::vector<SlotInfo> out;
        for (std::size_t i = 0; i < kInlineCapacity; ++i) {
            TxnId v = slots_[i].txnId.load(std::memory_order_relaxed);
            if (v != kEmptySlot) {
                out.push_back(SlotInfo{static_cast<ReaderHandle>(i), v});
            }
        }
        std::lock_guard<std::mutex> lock(overflowMutex_);
        for (const auto& [h, v] : overflow_) {
            out.push_back(SlotInfo{h, v});
        }
        return out;
    }

    std::size_t activeReaderCount() const {
        std::size_t n = 0;
        for (std::size_t i = 0; i < kInlineCapacity; ++i) {
            if (slots_[i].txnId.load(std::memory_order_relaxed) != kEmptySlot) ++n;
        }
        std::lock_guard<std::mutex> lock(overflowMutex_);
        return n + overflow_.size();
    }
    // Lightweight diagnostics -- cheap enough to expose on a telemetry/stats
    // API without a dedicated instrumentation pass later.
    struct Stats {
        std::size_t inlineUsed = 0;
        std::size_t inlineCapacity = kInlineCapacity;
        std::size_t overflowUsed = 0;
    };
    Stats stats() const {
        Stats s;
        for (std::size_t i = 0; i < kInlineCapacity; ++i) {
            if (slots_[i].txnId.load(std::memory_order_relaxed) != kEmptySlot) ++s.inlineUsed;
        }
        std::lock_guard<std::mutex> lock(overflowMutex_);
        s.overflowUsed = overflow_.size();
        return s;
    }
private:
    static constexpr TxnId kEmptySlot = kInvalidTxnId; // 0; never a real snapshot txnId
    struct alignas(64) Slot {
        std::atomic<TxnId> txnId{kEmptySlot};
    };
    static_assert(sizeof(Slot) == 64, "Slot must occupy exactly one cache line to avoid false sharing");
    std::array<Slot, kInlineCapacity> slots_;
    mutable std::mutex overflowMutex_;
    std::unordered_map<ReaderHandle, TxnId> overflow_;
    ReaderHandle nextOverflowId_ = 0;
};
} // namespace annpocketdb::core
