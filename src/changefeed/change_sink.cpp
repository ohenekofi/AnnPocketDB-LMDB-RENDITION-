//
// AnnPocketDb :: changefeed/change_sink.cpp
//
// ChangeSink and the durable-log encoding helpers it needs -- split out of
// changefeed.cpp so this part is *always* compiled, unlike the rest of the
// changefeed module (delivery backends, DurableChangeFeed, factories),
// which stays behind ANNPOCKETDB_WITH_CHANGEFEED.
//
// Why: WriteTransaction::put/remove/commit (src/txn/transaction.cpp, always
// kernel tier) call changeSink_->record/flush/notifyCommitted directly and
// unconditionally whenever a sink happens to be attached. Those calls need
// ChangeSink's symbols to link regardless of whether ANNPOCKETDB_WITH_
// CHANGEFEED is on -- Environment::open() (src/api/environment.cpp) already
// refuses to ever attach a sink when the flag is off (returns
// Status::NotSupported if enableChangefeed is requested), so ChangeSink's
// methods are simply never *called* in that configuration, but the linker
// still needs them to *exist*. ChangeSink::notifyCommitted()'s call to
// delivery_->publish(ev) is a virtual call through the abstract IChangeFeed
// interface (declared in changefeed.hpp, always visible) -- it needs no
// concrete delivery-backend symbol to link, only to actually run, and it
// never runs here since delivery_ is only ever non-null when a concrete
// backend was actually constructed (which only happens when the flag is on).
//
#include "annpocketdb/changefeed/changefeed.hpp"

#include <cstring>

#include "annpocketdb/txn/transaction.hpp"

namespace annpocketdb::changefeed {

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
        delivery_->publish(ev);
    }
    committedBatch_.clear();
}

}  // namespace annpocketdb::changefeed
