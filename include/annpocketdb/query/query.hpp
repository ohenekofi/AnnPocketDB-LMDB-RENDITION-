#pragma once
//
// AnnPocketDb :: query/query.hpp
//
// Query engine over documents and secondary indexes.
//
// Planner (cost-based among usable indexes):
//   - Collect candidate indexes whose fieldPath matches a predicate field.
//   - Score: Eq << Prefix << closed range (Ge/Gt + Le/Lt) << open range << scan.
//   - Pick the lowest-cost candidate; ties prefer Eq over Prefix over range.
//
// Access paths:
//   - TableScan: full primary cursor.
//   - IndexSeek: point / prefix / ordered range on a secondary index.
//
// Projection pushdown:
//   Query::project lists field paths. When non-empty, results carry only those
//   fields (decoded via the codec) instead of the full raw document body.
//
// Parallel residual evaluation:
//   Query::parallelWorkers > 1 enables multi-threaded residual predicate
//   checks over batches of candidate primary keys (index or scan produced).
//
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "annpocketdb/document/document.hpp"
#include "annpocketdb/index/index.hpp"
#include "annpocketdb/txn/transaction.hpp"
#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

namespace annpocketdb::query {

enum class Op : std::uint8_t {
    Eq = 1, Ne = 2, Lt = 3, Le = 4, Gt = 5, Ge = 6,
    Prefix = 7, Exists = 8,
};

struct Predicate {
    std::string field;
    Op op = Op::Eq;
    std::string value;
};

struct Query {
    std::vector<Predicate> predicates;
    std::uint64_t limit = 0;                    // 0 = unlimited
    std::vector<std::string> project;           // empty = return full document
    std::uint32_t parallelWorkers = 1;          // 1 = serial residuals
};

struct ProjectedField {
    std::string path;
    std::string value;
};

struct QueryResult {
    ByteBuffer primaryKey;
    document::Document doc;                     // empty raw if projected-only
    std::vector<ProjectedField> projected;      // filled when Query fore.project non-empty
};

enum class AccessPath : std::uint8_t {
    TableScan = 0,
    IndexPoint = 1,    // Eq
    IndexPrefix = 2,   // Prefix
    IndexRange = 3,    // Lt/Le/Gt/Ge (possibly combined)
};

struct QueryPlan {
    AccessPath path = AccessPath::TableScan;
    std::string indexName;
    std::string fieldPath;
    std::size_t drivenByPred = static_cast<std::size_t>(-1);
    // Range bounds when path == IndexRange (inclusive flags).
    bool hasLower = false;
    bool lowerInclusive = true;
    std::string lower;
    bool hasUpper = false;
    bool upperInclusive = true;
    std::string upper;
    // Heuristic cost (lower is better). Exposed for tests/explain.
    std::int64_t cost = 0;
};

class IQueryEngine {
public:
    virtual ~IQueryEngine() = default;
    virtual Result<std::vector<QueryResult>> execute(txn::ReadTransaction& txn,
                                                      const Query& q) = 0;
    virtual Result<std::uint64_t> countEstimate(txn::ReadTransaction& txn,
                                                 const Query& q) = 0;
    virtual QueryPlan explain(const Query& q) const { (void)q; return {}; }
};

class QueryEngine : public IQueryEngine {
public:
    QueryEngine(document::IDocumentCodec& codec, index::IndexManager* indexes = nullptr);

    Result<std::vector<QueryResult>> execute(txn::ReadTransaction& txn,
                                              const Query& q) override;
    Result<std::uint64_t> countEstimate(txn::ReadTransaction& txn,
                                         const Query& q) override;
    QueryPlan explain(const Query& q) const override;

private:
    struct MatchCtx {
        ByteView primaryKey;
        ByteView rawDoc;
    };
    struct CandidateKey {
        ByteBuffer primaryKey;
    };

    QueryPlan planQuery(const Query& q) const;
    Result<bool> matchAll(MatchCtx& ctx, const Query& q,
                          const std::vector<std::size_t>& skipPreds = {}) const;
    static bool compareValues(Op op, std::string_view fieldVal, std::string_view predVal);
    Status projectRow(ByteView primaryKey, ByteView rawDoc, const Query& q,
                      QueryResult& out) const;

    Result<std::vector<QueryResult>> executeTableScan(txn::ReadTransaction& txn,
                                                       const Query& q) const;
    Result<std::vector<QueryResult>> executeIndex(txn::ReadTransaction& txn,
                                                   const Query& q,
                                                   const QueryPlan& plan) const;

    // Collect primary keys from an index range/point, then filter + project.
    Result<std::vector<CandidateKey>> collectIndexKeys(txn::ReadTransaction& txn,
                                                        const QueryPlan& plan,
                                                        std::uint64_t maxKeys) const;
    Result<std::vector<QueryResult>> filterAndProject(txn::ReadTransaction& txn,
                                                      const Query& q,
                                                      const QueryPlan& plan,
                                                      std::vector<CandidateKey> keys) const;

    document::IDocumentCodec* codec_ = nullptr;
    index::IndexManager* indexes_ = nullptr;
};

class TableScanEngine : public IQueryEngine {
public:
    TableScanEngine();
    Result<std::vector<QueryResult>> execute(txn::ReadTransaction& txn, const Query& q) override;
    Result<std::uint64_t> countEstimate(txn::ReadTransaction& txn, const Query& q) override;
    QueryPlan explain(const Query& q) const override;
private:
    std::unique_ptr<document::IDocumentCodec> codec_;
    std::unique_ptr<QueryEngine> impl_;
};

Result<std::unique_ptr<IQueryEngine>> createQueryEngine();
Result<std::unique_ptr<IQueryEngine>> createQueryEngine(document::IDocumentCodec& codec,
                                                        index::IndexManager* indexes);

}  // namespace annpocketdb::query
