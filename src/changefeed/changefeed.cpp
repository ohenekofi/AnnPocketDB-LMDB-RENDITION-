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
void InProcessChangeFeed::emit(const ChangeEvent& event) {
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
void AsyncChangeFeed::emit(const ChangeEvent& event) {
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

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

bool isSystemKey(ByteView key) {
    constexpr std::string_view prefix = "__sys_";
    if (key.size() < prefix.size()) return false;
    return std::memcmp(key.data(), prefix.data(), prefix.size()) == 0;
}

ByteBuffer makeLogKey(Sequence seq) {
    // Prefix + 16 uppercase hex digits (fixed width for ordered scan).
    char buf[16];
    static const char* hex = "0123456789ABCDEF";
    for (int i = 15; i >= 0; --i) {
        buf[i] = hex[seq & 0xF];
        seq >>= 4;
    }
    ByteBuffer key;
    key.reserve(kChangeLogPrefix.size() + 16);
    key.insert(key.end(),
               reinterpret_cast<const std::byte*>(kChangeLogPrefix.data()),
               reinterpret_cast<const std::byte*>(kChangeLogPrefix.data()) + kChangeLogPrefix.size());
    key.insert(key.end(),
               reinterpret_cast<const std::byte*>(buf),
               reinterpret_cast<const std::byte*>(buf) + 16);
    return key;
}

bool parseLogKey(ByteView key, Sequence& seq) {
    if (key.size() != kChangeLogPrefix.size() + 16) return false;
    if (std::memcmp(key.data(), kChangeLogPrefix.data(), kChangeLogPrefix.size()) != 0) return false;
    Sequence s = 0;
    const auto* p = reinterpret_cast<const char*>(key.data() + kChangeLogPrefix.size());
    for (int i = 0; i < 16; ++i) {
        char c = p[i];
        Sequence nibble = 0;
        if (c >= '0' && c <= '9') nibble = static_cast<Sequence>(c - '0');
        else if (c >= 'A' && c <= 'F') nibble = static_cast<Sequence>(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') nibble = static_cast<Sequence>(c - 'a' + 10);
        else return false;
        s = (s << 4) | nibble;
    }
    seq = s;
    return true;
}

Status encodeChangeEvent(const ChangeEvent& ev, ByteBuffer& out) {
    // Layout: type u8 | txnId u64 | seq u64 |
    //         keyLen u32 | key | oldLen u32 | old | newLen u32 | new
    auto putU32 = [&](std::uint32_t v) {
        std::size_t o = out.size();
        out.resize(o + 4);
        std::memcpy(out.data() + o, &v, 4);
    };
    auto putU64 = [&](std::uint64_t v) {
        std::size_t o = out.size();
        out.resize(o + 8);
        std::memcpy(out.data() + o, &v, 8);
    };
    auto putBytes = [&](ByteView b) {
        putU32(static_cast<std::uint32_t>(b.size()));
        out.insert(out.end(), b.begin(), b.end());
    };

    out.clear();
    out.push_back(static_cast<std::byte>(ev.type));
    putU64(ev.txnId);
    putU64(ev.sequence);
    putBytes(ByteView(ev.key.data(), ev.key.size()));
    putBytes(ByteView(ev.oldValue.data(), ev.oldValue.size()));
    putBytes(ByteView(ev.newValue.data(), ev.newValue.size()));
    return Status::OK();
}

Status decodeChangeEvent(ByteView in, ChangeEvent& out) {
    auto need = [&](std::size_t n) -> Status {
        if (in.size() < n) return Status::Corruption("change event truncated");
        return Status::OK();
    };
    Status s = need(1 + 8 + 8);
    if (!s.ok()) return s;
    out.type = static_cast<ChangeType>(static_cast<std::uint8_t>(in[0]));
    std::memcpy(&out.txnId, in.data() + 1, 8);
    std::memcpy(&out.sequence, in.data() + 9, 8);
    std::size_t off = 17;
    auto readBuf = [&](ByteBuffer& dest) -> Status {
        s = need(off + 4);
        if (!s.ok()) return s;
        std::uint32_t len = 0;
        std::memcpy(&len, in.data() + off, 4);
        off += 4;
        s = need(off + len);
        if (!s.ok()) return s;
        dest.assign(in.data() + off, in.data() + off + len);
        off += len;
        return Status::OK();
    };
    s = readBuf(out.key); if (!s.ok()) return s;
    s = readBuf(out.oldValue); if (!s.ok()) return s;
    s = readBuf(out.newValue); if (!s.ok()) return s;
    return Status::OK();
}

// ---------------------------------------------------------------------------
// ChangeSink
// ---------------------------------------------------------------------------

ChangeSink::ChangeSink(IChangeFeed* delivery) : delivery_(delivery) {}

void ChangeSink::record(ChangeType type, TxnId txnId,
                         ByteView key, ByteView oldValue, ByteView newValue) {
    if (isSystemKey(key)) return;
    ChangeEvent ev;
    ev.type = type;
    ev.txnId = txnId;
    ev.key.assign(key.begin(), key.end());
    ev.oldValue.assign(oldValue.begin(), oldValue.end());
    ev.newValue.assign(newValue.begin(), newValue.end());
    pending_.push_back(std::move(ev));
}

void ChangeSink::clear() {
    pending_.clear();
    committedBatch_.clear();
}

Status ChangeSink::flush(txn::WriteTransaction& txn) {
    committedBatch_.clear();
    if (pending_.empty()) return Status::OK();

    // Load current sequence from meta key.
    Sequence next = 1;
    auto meta = txn.get(toByteView(kChangeMetaKey));
    if (meta.ok() && meta.value().size() >= 8) {
        Sequence last = 0;
        std::memcpy(&last, meta.value().data(), 8);
        next = last + 1;
    } else if (!meta.ok() && meta.status().code() != StatusCode::NotFound) {
        return meta.status();
    }

    for (auto& ev : pending_) {
        ev.sequence = next++;
        ByteBuffer payload;
        Status s = encodeChangeEvent(ev, payload);
        if (!s.ok()) return s;
        ByteBuffer logKey = makeLogKey(ev.sequence);
        s = txn.put(toByteView(logKey), toByteView(payload), true);
        if (!s.ok()) return s;
        committedBatch_.push_back(ev);
    }

    Sequence lastSeq = committedBatch_.back().sequence;
    ByteBuffer metaVal(8);
    std::memcpy(metaVal.data(), &lastSeq, 8);
    Status s = txn.put(toByteView(kChangeMetaKey), toByteView(metaVal), true);
    if (!s.ok()) return s;

    pending_.clear();
    return Status::OK();
}

void ChangeSink::notifyCommitted() {
    if (delivery_ == nullptr) return;
    for (const auto& ev : committedBatch_) {
        delivery_->emit(ev);
    }
    committedBatch_.clear();
}

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
