#pragma once
//
// AnnPocketDb :: changefeed/changefeed.hpp
//
// Ordered, durable change stream derived from committed transactions.
//
// Design:
//   - WriteTransaction records put/remove on the master keyspace (skipping
//     __sys_* keys) into a pending list when a ChangeSink is attached.
//   - On commit, the sink appends events to a durable log in the master
//     keyspace (__sys_cf_log__/<seq>) and advances __sys_cf_meta__, then the
//     meta page is flipped. Subscribers are notified only after a successful
//     commit so observers never see uncommitted data.
//   - Resume tokens are opaque uint64 sequence numbers (last-seen seq).
//   - InProcess / Async feeds are delivery backends; DurableChangeFeed
//     combines the log with a delivery backend.
//
// Cross-process: any process can open the same DB and pollFrom(token) to
// read the durable log; live push remains in-process only.
//
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

namespace annpocketdb::txn { class WriteTransaction; class ReadTransaction; }

namespace annpocketdb::changefeed {

enum class ChangeType : std::uint8_t {
    Insert = 1,
    Update = 2,
    Delete = 3,
};

// Monotonic log sequence; also used as the resume token (last-seen).
using Sequence = std::uint64_t;
constexpr Sequence kInvalidSequence = 0;

struct ChangeEvent {
    Sequence sequence = kInvalidSequence;
    ChangeType type = ChangeType::Insert;
    TxnId txnId = kInvalidTxnId;
    ByteBuffer key;
    ByteBuffer oldValue;
    ByteBuffer newValue;
};

using ChangeHandler = std::function<void(const ChangeEvent&)>;
using ResumeToken = Sequence;  // last fully processed sequence

// ---------------------------------------------------------------------------
// Delivery backends (in-process)
// ---------------------------------------------------------------------------

class IChangeFeed {
public:
    virtual ~IChangeFeed() = default;
    virtual void subscribe(ChangeHandler handler) = 0;
    virtual void unsubscribe() = 0;
    virtual void emit(const ChangeEvent& event) = 0;
};

class InProcessChangeFeed : public IChangeFeed {
public:
    void subscribe(ChangeHandler handler) override;
    void unsubscribe() override;
    void emit(const ChangeEvent& event) override;
private:
    mutable std::mutex mutex_;
    ChangeHandler handler_;
    bool hasHandler_ = false;
};

class AsyncChangeFeed : public IChangeFeed {
public:
    AsyncChangeFeed();
    ~AsyncChangeFeed() override;
    void subscribe(ChangeHandler handler) override;
    void unsubscribe() override;
    void emit(const ChangeEvent& event) override;
private:
    void run();
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<ChangeEvent> queue_;
    ChangeHandler handler_;
    std::atomic<bool> running_{true};
    std::thread thread_;
};

// ---------------------------------------------------------------------------
// Durable log + sink hooked into WriteTransaction
// ---------------------------------------------------------------------------

// System keys (not emitted as user changes).
inline constexpr std::string_view kChangeLogPrefix = "__sys_cf_log__/";
inline constexpr std::string_view kChangeMetaKey   = "__sys_cf_meta__";

bool isSystemKey(ByteView key);

// Encode / decode a single log value.
Status encodeChangeEvent(const ChangeEvent& ev, ByteBuffer& out);
Status decodeChangeEvent(ByteView in, ChangeEvent& out);

// Build the master-keyspace key for a log sequence number.
ByteBuffer makeLogKey(Sequence seq);
// Parse sequence from a log key; returns false if not a log key.
bool parseLogKey(ByteView key, Sequence& seq);

// Attached to a WriteTransaction for its lifetime.
class ChangeSink {
public:
    explicit ChangeSink(IChangeFeed* delivery = nullptr);

    // Record a user-visible change (ignored for system keys).
    void record(ChangeType type, TxnId txnId,
                ByteView key, ByteView oldValue, ByteView newValue);

    // Append pending events to the durable log via the write txn (must run
    // before PageManager::commit). Assigns sequence numbers.
    Status flush(txn::WriteTransaction& txn);

    // Notify delivery backend after a successful commit.
    void notifyCommitted();

    void clear();
    std::size_t pendingCount() const { return pending_.size(); }

private:
    IChangeFeed* delivery_ = nullptr;
    std::vector<ChangeEvent> pending_;
    std::vector<ChangeEvent> committedBatch_;
};

// High-level feed: durable poll + optional live delivery.
class DurableChangeFeed {
public:
    explicit DurableChangeFeed(std::unique_ptr<IChangeFeed> delivery);

    IChangeFeed& delivery() { return *delivery_; }
    ChangeSink* sink() { return &sink_; }

    void subscribe(ChangeHandler handler);
    void unsubscribe();

    // Read durable events with sequence > afterToken, up to limit (0 = all).
    // Uses a ReadTransaction supplied by the caller (snapshot isolation).
    Result<std::vector<ChangeEvent>> pollFrom(txn::ReadTransaction& txn,
                                               ResumeToken afterToken,
                                               std::uint64_t limit = 0);

    // Latest durable sequence visible in this snapshot (0 if empty).
    Result<Sequence> latestSequence(txn::ReadTransaction& txn);

private:
    std::unique_ptr<IChangeFeed> delivery_;
    ChangeSink sink_;
};

Result<std::unique_ptr<IChangeFeed>> createChangeFeed(bool async = false);
Result<std::unique_ptr<DurableChangeFeed>> createDurableChangeFeed(bool async = false);

}  // namespace annpocketdb::changefeed
