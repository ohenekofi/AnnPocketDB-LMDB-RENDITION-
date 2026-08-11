#pragma once
//
// AnnPocketDb :: core/writer_gate.hpp
//
// Single-writer gate used by WriteTransaction.
//
//   multiProcess == true  → sidecar .lock file (InterprocessLock)
//   multiProcess == false → in-process std::mutex + ReaderTable (no .lock file)
//
#include <mutex>
#include <cstdint>
#include <vector>
#include <climits>

#include "annpocketdb/core/reader_table.hpp"
#include "annpocketdb/platform/interprocess_lock.hpp"
#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

namespace annpocketdb::core {

class WriterGate {
public:
    WriterGate() = default;

    Status open(const std::filesystem::path& dataPath, bool readOnly, bool multiProcess,
                std::uint32_t maxReaders = 256) {
        multiProcess_ = multiProcess;
        readOnly_ = readOnly;
        maxReaders_ = maxReaders == 0 ? 256 : maxReaders;
        if (multiProcess_ && maxReaders_ > platform::kSharedReaderSlots) {
            maxReaders_ = static_cast<std::uint32_t>(platform::kSharedReaderSlots);
        }
        if (!multiProcess_) return Status::OK();
        auto r = platform::InterprocessLock::open(dataPath, readOnly);
        if (!r) return r.status();
        ip_ = r.moveValue();
        return Status::OK();
    }

    bool multiProcess() const { return multiProcess_; }

    // BasicLockable for std::unique_lock
    void lock() {
        if (multiProcess_) {
            ip_.lock();
            return;
        }
        localWriter_.lock();
    }
    void unlock() {
        if (multiProcess_) {
            ip_.unlock();
            return;
        }
        localWriter_.unlock();
    }
    bool try_lock() {
        if (multiProcess_) return ip_.try_lock();
        return localWriter_.try_lock();
    }

    ReaderHandle registerReader(TxnId snapshotTxnId) {
        if (activeReaderCount() >= maxReaders_) {
            return static_cast<ReaderHandle>(-1);  // Busy — callers map to Busy status
        }
        if (multiProcess_) {
            return static_cast<ReaderHandle>(ip_.registerReader(snapshotTxnId));
        }
        return localReaders_.registerReader(snapshotTxnId);
    }

    void unregisterReader(ReaderHandle handle) {
        if (multiProcess_) {
            ip_.unregisterReader(static_cast<platform::ReaderHandle>(handle));
            return;
        }
        localReaders_.unregisterReader(handle);
    }

    TxnId oldestActiveSnapshot(TxnId fallbackIfNoneActive) const {
        if (multiProcess_) return ip_.oldestActiveSnapshot(fallbackIfNoneActive);
        return localReaders_.oldestActiveSnapshot(fallbackIfNoneActive);
    }

    std::size_t activeReaderCount() const {
        if (multiProcess_) return ip_.activeReaderCount();
        return localReaders_.activeReaderCount();
    }
    static constexpr std::size_t maxReadersMultiProcess() {
        return platform::kSharedReaderSlots;
    }
    std::size_t maxReaders() const { return maxReaders_; }
    std::size_t readerCheck() {
        if (multiProcess_) return ip_.readerCheck();
        return 0;  // in-process slots cannot be "stale" across process death
    }
    std::vector<platform::InterprocessLock::ReaderSlotInfo> readerList() const {
        if (multiProcess_) return ip_.readerList();
        std::vector<platform::InterprocessLock::ReaderSlotInfo> out;
        for (const auto& s : localReaders_.listReaders()) {
            platform::InterprocessLock::ReaderSlotInfo info;
            info.slot = static_cast<std::uint32_t>(s.handle);
            info.pid = 0;
            info.snapshotTxnId = s.snapshotTxnId;
            out.push_back(info);
        }
        return out;
    }

private:
    bool multiProcess_ = true;
    bool readOnly_ = false;
    std::uint32_t maxReaders_ = 256;
    platform::InterprocessLock ip_;
    ReaderTable localReaders_;
    std::mutex localWriter_;
};

}  // namespace annpocketdb::core
