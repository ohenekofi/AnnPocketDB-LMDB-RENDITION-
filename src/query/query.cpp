#include "annpocketdb/query/query.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>

#include "annpocketdb/btree/cursor.hpp"

namespace annpocketdb::query {

namespace {

document::FieldPath splitPath(const std::string& field) {
    document::FieldPath path;
    std::size_t start = 0;
    while (start <= field.size()) {
        std::size_t dot = field.find('.', start);
        if (dot == std::string::npos) {
            if (start < field.size() || path.empty()) path.push_back(field.substr(start));
            break;
        }
        path.push_back(field.substr(start, dot - start));
        start = dot + 1;
    }
    path.erase(std::remove_if(path.begin(), path.end(),
                              [](const std::string& s) { return s.empty(); }),
               path.end());
    return path;
}

// Cost model (lower is better). Absolute scale is arbitrary; relative order matters.
constexpr std::int64_t kCostEq = 10;
constexpr std::int64_t kCostPrefix = 100;
constexpr std::int64_t kCostClosedRange = 500;
constexpr std::int64_t kCostOpenRange = 2000;
constexpr std::int64_t kCostTableScan = 100000;

bool fieldMatchesIndex(const std::string& predField, const std::string& indexName,
                       const std::string& fieldPath) {
    return predField == fieldPath || predField == indexName;
}

}  // namespace

QueryEngine::QueryEngine(document::IDocumentCodec& codec, index::IndexManager* indexes)
    : codec_(&codec), indexes_(indexes) {}

QueryPlan QueryEngine::planQuery(const Query& q) const {
    QueryPlan best;
    best.path = AccessPath::TableScan;
    best.cost = kCostTableScan;
    if (indexes_ == nullptr) return best;

    // Group predicates by field.
    // For each registered index, try to build a point/prefix/range plan.
    for (const std::string& name : indexes_->listIndexes()) {
        auto metaR = indexes_->meta(name);
        if (!metaR) continue;
        const auto& meta = metaR.value();

        std::vector<std::size_t> onField;
        for (std::size_t i = 0; i < q.predicates.size(); ++i) {
            if (fieldMatchesIndex(q.predicates[i].field, name, meta.fieldPath)) {
                onField.push_back(i);
            }
        }
        if (onField.empty()) continue;

        // Prefer Eq, then Prefix, then range bounds on this field.
        std::optional<std::size_t> eqPred, prefixPred;
        bool hasLower = false, hasUpper = false;
        bool lowerIncl = true, upperIncl = true;
        std::string lower, upper;
        std::size_t rangeDriver = static_cast<std::size_t>(-1);

        for (std::size_t i : onField) {
            const Predicate& p = q.predicates[i];
            switch (p.op) {
                case Op::Eq:
                    if (!eqPred.has_value()) eqPred = i;
                    break;
                case Op::Prefix:
                    if (!prefixPred.has_value()) prefixPred = i;
                    break;
                case Op::Ge:
                    if (!hasLower || p.value > lower) {
                        hasLower = true; lowerIncl = true; lower = p.value; rangeDriver = i;
                    }
                    break;
                case Op::Gt:
                    if (!hasLower || p.value >= lower) {
                        hasLower = true; lowerIncl = false; lower = p.value; rangeDriver = i;
                    }
                    break;
                case Op::Le:
                    if (!hasUpper || p.value < upper) {
                        hasUpper = true; upperIncl = true; upper = p.value; rangeDriver = i;
                    }
                    break;
                case Op::Lt:
                    if (!hasUpper || p.value <= upper) {
                        hasUpper = true; upperIncl = false; upper = p.value; rangeDriver = i;
                    }
                    break;
                default:
                    break;
            }
        }

        QueryPlan cand;
        cand.indexName = name;
        cand.fieldPath = meta.fieldPath;

        if (eqPred.has_value()) {
            cand.path = AccessPath::IndexPoint;
            cand.drivenByPred = *eqPred;
            cand.cost = kCostEq;
            cand.hasLower = cand.hasUpper = true;
            cand.lowerInclusive = cand.upperInclusive = true;
            cand.lower = cand.upper = q.predicates[*eqPred].value;
        } else if (prefixPred.has_value()) {
            cand.path = AccessPath::IndexPrefix;
            cand.drivenByPred = *prefixPred;
            cand.cost = kCostPrefix;
            cand.hasLower = true;
            cand.lowerInclusive = true;
            cand.lower = q.predicates[*prefixPred].value;
            // Upper bound = prefix + 0xFF… is handled during scan via prefix check.
        } else if (hasLower || hasUpper) {
            cand.path = AccessPath::IndexRange;
            cand.drivenByPred = rangeDriver;
            cand.hasLower = hasLower;
            cand.hasUpper = hasUpper;
            cand.lowerInclusive = lowerIncl;
            cand.upperInclusive = upperIncl;
            cand.lower = lower;
            cand.upper = upper;
            cand.cost = (hasLower && hasUpper) ? kCostClosedRange : kCostOpenRange;
        } else {
            continue;  // only Ne/Exists on this field — not index-friendly alone
        }

        if (cand.cost < best.cost) best = cand;
    }
    return best;
}

QueryPlan QueryEngine::explain(const Query& q) const { return planQuery(q); }

bool QueryEngine::compareValues(Op op, std::string_view fieldVal, std::string_view predVal) {
    switch (op) {
        case Op::Eq: return fieldVal == predVal;
        case Op::Ne: return fieldVal != predVal;
        case Op::Lt: return fieldVal < predVal;
        case Op::Le: return fieldVal <= predVal;
        case Op::Gt: return fieldVal > predVal;
        case Op::Ge: return fieldVal >= predVal;
        case Op::Prefix:
            return fieldVal.size() >= predVal.size() &&
                   fieldVal.substr(0, predVal.size()) == predVal;
        case Op::Exists: return true;
    }
    return false;
}

Result<bool> QueryEngine::matchAll(MatchCtx& ctx, const Query& q,
                                    const std::vector<std::size_t>& skipPreds) const {
    auto skipped = [&](std::size_t i) {
        return std::find(skipPreds.begin(), skipPreds.end(), i) != skipPreds.end();
    };
    for (std::size_t i = 0; i < q.predicates.size(); ++i) {
        if (skipped(i)) continue;
        const Predicate& pred = q.predicates[i];
        if (pred.field.empty()) {
            std::string_view raw = toStringView(ctx.rawDoc);
            if (!compareValues(pred.op, raw, pred.value)) return Result<bool>(false);
            continue;
        }
        document::FieldPath path = splitPath(pred.field);
        std::string extracted;
        Status es = codec_->extractField(ctx.rawDoc, path, extracted);
        if (pred.op == Op::Exists) {
            if (!es.ok()) {
                if (es.code() == StatusCode::NotFound) return Result<bool>(false);
                return es;
            }
            continue;
        }
        if (!es.ok()) {
            if (es.code() == StatusCode::NotFound) return Result<bool>(false);
            return es;
        }
        if (!compareValues(pred.op, extracted, pred.value)) return Result<bool>(false);
    }
    return Result<bool>(true);
}

Status QueryEngine::projectRow(ByteView primaryKey, ByteView rawDoc, const Query& q,
                                QueryResult& out) const {
    out.primaryKey.assign(primaryKey.begin(), primaryKey.end());
    out.projected.clear();
    if (q.project.empty()) {
        out.doc = document::Document(ByteBuffer(rawDoc.begin(), rawDoc.end()));
        return Status::OK();
    }
    // Projection pushdown: only extract requested fields; leave doc.raw empty.
    out.doc = document::Document();
    for (const auto& pathStr : q.project) {
        document::FieldPath path = splitPath(pathStr);
        std::string val;
        Status s = codec_->extractField(rawDoc, path, val);
        if (!s.ok()) {
            if (s.code() == StatusCode::NotFound) {
                out.projected.push_back({pathStr, {}});
                continue;
            }
            return s;
        }
        out.projected.push_back({pathStr, std::move(val)});
    }
    return Status::OK();
}

Result<std::vector<QueryEngine::CandidateKey>> QueryEngine::collectIndexKeys(
    txn::ReadTransaction& txn, const QueryPlan& plan, std::uint64_t maxKeys) const {
    (void)txn;
    std::vector<CandidateKey> keys;
    if (indexes_ == nullptr) return Result<std::vector<CandidateKey>>(std::move(keys));

    auto idxR = indexes_->openIndex(plan.indexName);
    if (!idxR) return idxR.status();
    index::Index idx = idxR.moveValue();

    if (plan.path != AccessPath::IndexPoint && plan.path != AccessPath::IndexPrefix &&
        plan.path != AccessPath::IndexRange) {
        return Result<std::vector<CandidateKey>>(std::move(keys));
    }

    // Predicate::value is always a plain string, but a secondary index is
    // populated via IndexManager::maintainOnPut -> document::extractIndexKeys
    // -> Value::toIndexBytes(), which prepends a one-byte type tag (5 for
    // String) before the raw field bytes -- not the bare bytes of the
    // predicate string. Re-encode plan.lower/upper the same way before
    // comparing against (or seeking on) the index's actual stored bytes, or
    // every comparison below silently mismatches on the leading tag byte.
    // This assumes the indexed field is String-valued, matching what
    // Predicate::value (always a std::string) can meaningfully express --
    // the residual filter path (compareValues, via extractField's canonical
    // string form) is untouched and stays correct for any value type.
    const ByteBuffer lowerBytes = document::Value::string(plan.lower).toIndexBytes();
    const ByteBuffer upperBytes = document::Value::string(plan.upper).toIndexBytes();

    // Cursor holds a reference member (const BTree&), so its defaulted
    // move-assignment is silently deleted by the compiler (a reference
    // can't be rebound) -- Result<Cursor> inherits that, so it must be
    // constructed once here rather than default-constructed and reassigned
    // per branch below.
    Result<btree::Cursor> curR = [&]() -> Result<btree::Cursor> {
        if (plan.path == AccessPath::IndexPoint || plan.path == AccessPath::IndexPrefix) {
            return idx.seekLowerBound(toByteView(lowerBytes));
        }
        // AccessPath::IndexRange
        if (plan.hasLower) {
            return idx.seekLowerBound(toByteView(lowerBytes));
        }
        return idx.openCursor();
    }();

    if (!curR) {
        if (curR.status().code() == StatusCode::NotFound)
            return Result<std::vector<CandidateKey>>(std::move(keys));
        return curR.status();
    }
    btree::Cursor cursor = curR.moveValue();

    // Exclusive lower bound: skip equal secondary keys.
    if (plan.path == AccessPath::IndexRange && plan.hasLower && !plan.lowerInclusive) {
        ByteView lo = toByteView(lowerBytes);
        while (cursor.valid()) {
            auto k = cursor.key();
            if (!k) return k.status();
            // Index cursors are opened without setFlags(DupSort) (see
            // Index::find/seekLowerBound), so key() already yields the plain
            // secondary key -- there is no [secondary][0x00][primary]
            // composite to split here; the primary key lives in the
            // cursor's *value*, matching Index::find's own read pattern.
            ByteView sec = k.value();
            if (compareBytes(sec, lo) > 0) break;
            auto n = cursor.next();
            if (!n) return n.status();
            if (!n.value()) return Result<std::vector<CandidateKey>>(std::move(keys));
        }
    }

    while (cursor.valid()) {
        auto k = cursor.key();
        if (!k) return k.status();
        ByteView sec = k.value();
        auto vv = cursor.valueView();
        if (!vv) return vv.status();
        ByteView pri = vv.value();

        bool stop = false;
        bool include = true;

        if (plan.path == AccessPath::IndexPoint) {
            ByteView eq = toByteView(lowerBytes);
            if (compareBytes(sec, eq) != 0) { stop = true; include = false; }
        } else if (plan.path == AccessPath::IndexPrefix) {
            ByteView pref = toByteView(lowerBytes);
            if (sec.size() < pref.size() ||
                compareBytes(ByteView(sec.data(), pref.size()), pref) != 0) {
                stop = true; include = false;
            }
        } else {
            if (plan.hasUpper) {
                ByteView hi = toByteView(upperBytes);
                int cmp = compareBytes(sec, hi);
                if (plan.upperInclusive ? (cmp > 0) : (cmp >= 0)) {
                    stop = true; include = false;
                }
            }
        }

        if (include) {
            CandidateKey ck;
            ck.primaryKey.assign(pri.begin(), pri.end());
            keys.push_back(std::move(ck));
            if (maxKeys > 0 && keys.size() >= maxKeys) break;
        }
        if (stop) break;

        auto n = cursor.next();
        if (!n) return n.status();
        if (!n.value()) break;
    }
    return Result<std::vector<CandidateKey>>(std::move(keys));
}

Result<std::vector<QueryResult>> QueryEngine::filterAndProject(
    txn::ReadTransaction& txn, const Query& q, const QueryPlan& plan,
    std::vector<CandidateKey> keys) const {

    std::vector<std::size_t> skip;
    if (plan.drivenByPred != static_cast<std::size_t>(-1)) skip.push_back(plan.drivenByPred);
    // Also skip other predicates on the same field that the range already enforces.
    if (indexes_ && !plan.fieldPath.empty()) {
        for (std::size_t i = 0; i < q.predicates.size(); ++i) {
            if (i == plan.drivenByPred) continue;
            const auto& p = q.predicates[i];
            if (p.field == plan.fieldPath || p.field == plan.indexName) {
                if (p.op == Op::Eq || p.op == Op::Prefix || p.op == Op::Lt || p.op == Op::Le ||
                    p.op == Op::Gt || p.op == Op::Ge) {
                    skip.push_back(i);
                }
            }
        }
    }

    std::vector<QueryResult> out;
    out.reserve(keys.size());

    auto processOne = [&](const CandidateKey& ck, std::vector<QueryResult>& local) -> Status {
        auto docR = txn.get(ByteView(ck.primaryKey.data(), ck.primaryKey.size()));
        if (!docR) {
            if (docR.status().code() == StatusCode::NotFound) return Status::OK();
            return docR.status();
        }
        ByteBuffer docBuf = docR.moveValue();
        MatchCtx ctx;
        ctx.primaryKey = ByteView(ck.primaryKey.data(), ck.primaryKey.size());
        ctx.rawDoc = ByteView(docBuf.data(), docBuf.size());
        auto m = matchAll(ctx, q, skip);
        if (!m) return m.status();
        if (!m.value()) return Status::OK();
        QueryResult qr;
        Status ps = projectRow(ctx.primaryKey, ctx.rawDoc, q, qr);
        if (!ps.ok()) return ps;
        local.push_back(std::move(qr));
        return Status::OK();
    };

    const std::uint32_t workers = std::max<std::uint32_t>(1, q.parallelWorkers);
    if (workers == 1 || keys.size() < 32) {
        for (const auto& ck : keys) {
            Status s = processOne(ck, out);
            if (!s.ok()) return s;
            if (q.limit > 0 && out.size() >= q.limit) break;
        }
    } else {
        // Parallel residual evaluation over key batches.
        std::mutex mu;
        std::atomic<std::size_t> next{0};
        std::atomic<bool> failed{false};
        Status firstErr = Status::OK();
        std::vector<std::thread> pool;
        for (std::uint32_t t = 0; t < workers; ++t) {
            pool.emplace_back([&] {
                std::vector<QueryResult> local;
                while (!failed.load(std::memory_order_acquire)) {
                    std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
                    if (i >= keys.size()) break;
                    if (q.limit > 0) {
                        std::lock_guard lock(mu);
                        if (out.size() >= q.limit) break;
                    }
                    Status s = processOne(keys[i], local);
                    if (!s.ok()) {
                        std::lock_guard lock(mu);
                        if (!failed.exchange(true)) firstErr = s;
                        return;
                    }
                }
                if (!local.empty()) {
                    std::lock_guard lock(mu);
                    for (auto& r : local) {
                        if (q.limit > 0 && out.size() >= q.limit) break;
                        out.push_back(std::move(r));
                    }
                }
            });
        }
        for (auto& th : pool) th.join();
        if (failed.load()) return firstErr;
    }
    return Result<std::vector<QueryResult>>(std::move(out));
}

Result<std::vector<QueryResult>> QueryEngine::executeIndex(txn::ReadTransaction& txn,
                                                            const Query& q,
                                                            const QueryPlan& plan) const {
    std::uint64_t maxKeys = q.limit > 0 ? q.limit * 4 + 64 : 0;  // soft cap before residuals
    auto keysR = collectIndexKeys(txn, plan, maxKeys);
    if (!keysR) return keysR.status();
    return filterAndProject(txn, q, plan, keysR.moveValue());
}

Result<std::vector<QueryResult>> QueryEngine::executeTableScan(txn::ReadTransaction& txn,
                                                                const Query& q) const {
    std::vector<CandidateKey> keys;
    auto cursor = txn.openCursor();
    auto r = cursor.first();
    if (!r) return r.status();
    while (cursor.valid()) {
        auto k = cursor.key();
        if (!k) return k.status();
        CandidateKey ck;
        ck.primaryKey.assign(k.value().begin(), k.value().end());
        keys.push_back(std::move(ck));
        auto n = cursor.next();
        if (!n) return n.status();
        if (!n.value()) break;
    }
    QueryPlan scanPlan;
    scanPlan.path = AccessPath::TableScan;
    return filterAndProject(txn, q, scanPlan, std::move(keys));
}

Result<std::vector<QueryResult>> QueryEngine::execute(txn::ReadTransaction& txn, const Query& q) {
    QueryPlan plan = planQuery(q);
    if (plan.path == AccessPath::TableScan) return executeTableScan(txn, q);
    return executeIndex(txn, q, plan);
}

Result<std::uint64_t> QueryEngine::countEstimate(txn::ReadTransaction& txn, const Query& q) {
    QueryPlan plan = planQuery(q);
    // Cheap path: pure index predicate set, no projection needed.
    if (plan.path != AccessPath::TableScan && q.predicates.size() == 1) {
        auto keysR = collectIndexKeys(txn, plan, 0);
        if (!keysR) return keysR.status();
        return Result<std::uint64_t>(static_cast<std::uint64_t>(keysR.value().size()));
    }
    auto r = execute(txn, q);
    if (!r) return r.status();
    return Result<std::uint64_t>(static_cast<std::uint64_t>(r.value().size()));
}

TableScanEngine::TableScanEngine()
    : codec_(std::make_unique<document::PlainTextCodec>()),
      impl_(std::make_unique<QueryEngine>(*codec_, nullptr)) {}

Result<std::vector<QueryResult>> TableScanEngine::execute(txn::ReadTransaction& txn, const Query& q) {
    return impl_->execute(txn, q);
}
Result<std::uint64_t> TableScanEngine::countEstimate(txn::ReadTransaction& txn, const Query& q) {
    return impl_->countEstimate(txn, q);
}
QueryPlan TableScanEngine::explain(const Query& q) const { return impl_->explain(q); }

Result<std::unique_ptr<IQueryEngine>> createQueryEngine() {
    return Result<std::unique_ptr<IQueryEngine>>(std::make_unique<TableScanEngine>());
}
Result<std::unique_ptr<IQueryEngine>> createQueryEngine(document::IDocumentCodec& codec,
                                                        index::IndexManager* indexes) {
    return Result<std::unique_ptr<IQueryEngine>>(std::make_unique<QueryEngine>(codec, indexes));
}

}  // namespace annpocketdb::query
