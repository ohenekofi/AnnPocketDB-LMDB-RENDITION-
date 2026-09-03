#include "annpocketdb/changefeed/changefeed.hpp"

#include <cstring>

#include "annpocketdb/txn/transaction.hpp"

namespace annpocketdb::changefeed {

// ---------------------------------------------------------------------------
// InProcess / Async delivery
// ---------------------------------------------------------------------------

void InProcessChangeFeed::subscribe(ChangeHandler handler) {
    std::lock_guard lock(mutex_);
    handler_ = std::move(handler);
    hasHandler_ = true;
}
void InProcessChangeFeed::unsubscribe() {
    std::lock_guard lock(mutex_);
    handler_ = nullptr;
    hasHandler_ = false;
}
void InProcessChangeFeed::publish(const ChangeEvent& event) {
    ChangeHandler h;
    {
        std::lock_guard lock(mutex_);
        if (!hasHandler_) return;
        h = handler_;
    }
    if (h) h(event);
}

AsyncChangeFeed::AsyncChangeFeed() : thread_([this] { run(); }) {}
AsyncChangeFeed::~AsyncChangeFeed() {
    running_ = false;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}
void AsyncChangeFeed::subscribe(ChangeHandler handler) {
    std::lock_guard lock(mutex_);
    handler_ = std::move(handler);
}
void AsyncChangeFeed::unsubscribe() {
    std::lock_guard lock(mutex_);
    handler_ = nullptr;
}
void AsyncChangeFeed::publish(const ChangeEvent& event) {
    {
        std::lock_guard lock(mutex_);
        queue_.push(event);
    }
    cv_.notify_one();
}
void AsyncChangeFeed::run() {
    while (running_) {
        ChangeEvent ev;
        ChangeHandler h;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return !queue_.empty() || !running_; });
            if (!running_ && queue_.empty()) return;
            if (queue_.empty()) continue;
            ev = std::move(queue_.front());
            queue_.pop();
            h = handler_;
        }
        if (h) h(ev);
    }
}

Result<std::unique_ptr<IChangeFeed>> createChangeFeed(bool async) {
    if (async) {
        return Result<std::unique_ptr<IChangeFeed>>(std::make_unique<AsyncChangeFeed>());
    }
    return Result<std::unique_ptr<IChangeFeed>>(std::make_unique<InProcessChangeFeed>());
}

// isSystemKey/makeLogKey/parseLogKey/encodeChangeEvent/decodeChangeEvent and
// ChangeSink itself now live in change_sink.cpp, which is always compiled
// (unlike this file) -- see that file's header comment for why.

// ---------------------------------------------------------------------------
// DurableChangeFeed
// ---------------------------------------------------------------------------

DurableChangeFeed::DurableChangeFeed(std::unique_ptr<IChangeFeed> delivery)
    : delivery_(std::move(delivery)), sink_(delivery_.get()) {}

void DurableChangeFeed::subscribe(ChangeHandler handler) {
    delivery_->subscribe(std::move(handler));
}
void DurableChangeFeed::unsubscribe() { delivery_->unsubscribe(); }

Result<Sequence> DurableChangeFeed::latestSequence(txn::ReadTransaction& txn) {
    auto meta = txn.get(toByteView(kChangeMetaKey));
    if (!meta) {
        if (meta.status().code() == StatusCode::NotFound) return Result<Sequence>(kInvalidSequence);
        return meta.status();
    }
    if (meta.value().size() < 8) return Status::Corruption("changefeed meta truncated");
    Sequence last = 0;
    std::memcpy(&last, meta.value().data(), 8);
    return Result<Sequence>(last);
}

Result<std::vector<ChangeEvent>> DurableChangeFeed::pollFrom(txn::ReadTransaction& txn,
                                                              ResumeToken afterToken,
                                                              std::uint64_t limit) {
    std::vector<ChangeEvent> out;
    auto cursor = txn.openCursor();
    ByteBuffer start = makeLogKey(afterToken + 1);
    auto sk = cursor.seek(toByteView(start));
    if (!sk) return sk.status();
    if (!sk.value()) {
        // seek may fail if empty; try first
        auto fr = cursor.first();
        if (!fr) return fr.status();
        if (!fr.value()) return Result<std::vector<ChangeEvent>>(std::move(out));
        // advance until past afterToken
    }

    // Position: if seek found a key, use it; else scan from first.
    if (!cursor.valid()) {
        auto fr = cursor.first();
        if (!fr) return fr.status();
    }

    while (cursor.valid()) {
        auto k = cursor.key();
        auto v = cursor.value();
        if (!k) return k.status();
        if (!v) return v.status();

        Sequence seq = 0;
        if (!parseLogKey(k.value(), seq)) {
            // Past the log keyspace (or into unrelated keys). Stop if prefix no longer matches.
            if (k.value().size() < kChangeLogPrefix.size() ||
                std::memcmp(k.value().data(), kChangeLogPrefix.data(), kChangeLogPrefix.size()) != 0) {
                break;
            }
            auto n = cursor.next();
            if (!n) return n.status();
            if (!n.value()) break;
            continue;
        }
        if (seq <= afterToken) {
            auto n = cursor.next();
            if (!n) return n.status();
            if (!n.value()) break;
            continue;
        }

        ChangeEvent ev;
        Status s = decodeChangeEvent(toByteView(v.value()), ev);
        if (!s.ok()) return s;
        ev.sequence = seq;
        out.push_back(std::move(ev));
        if (limit > 0 && out.size() >= limit) break;

        auto n = cursor.next();
        if (!n) return n.status();
        if (!n.value()) break;
    }
    return Result<std::vector<ChangeEvent>>(std::move(out));
}

Result<std::unique_ptr<DurableChangeFeed>> createDurableChangeFeed(bool async) {
    auto backend = createChangeFeed(async);
    if (!backend) return backend.status();
    return Result<std::unique_ptr<DurableChangeFeed>>(
        std::make_unique<DurableChangeFeed>(backend.moveValue()));
}

}  // namespace annpocketdb::changefeed
