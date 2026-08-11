// Coverage for query::Query/QueryEngine/TableScanEngine (previously
// untested). Query is a plain struct (predicates/limit/project/
// parallelWorkers), not a fluent builder -- populated directly here.
//
// Two independent databases are seeded with the *same* logical dataset
// (40 docs, "category" in {cat0..cat4} x8 each, "score" = 0..39):
//   - scan.apdb: no IndexManager attached -- exercises the TableScan path.
//   - idx.apdb: an index on "category" created *before* any documents are
//     inserted (IndexManager has no retroactive backfill -- only
//     DocumentStore puts made *after* attaching see automatic maintenance),
//     so every doc lands in the index as it's written.
// Kept separate rather than sharing one database because a real TableScan
// walks the *entire* master keyspace -- including IndexManager's own
// "__sys_idx_catalog__" catalog key once an index exists -- which is not a
// valid document and would make table-scan queries fail if run against the
// same keyspace an index was created in.
#include "test_common.hpp"

#include <cstdio>
#include <set>
#include <string>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/document/document.hpp"
#include "annpocketdb/index/index.hpp"
#include "annpocketdb/query/query.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;
using namespace annpocketdb::document;
using namespace annpocketdb::index;
using namespace annpocketdb::query;

static std::string keyFor(int i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "doc%02d", i);
    return buf;
}

static Value docFor(int i) {
    Value::Map m;
    char cat[8];
    std::snprintf(cat, sizeof(cat), "cat%d", i % 5);
    m["category"] = Value::string(cat);
    m["score"] = Value::integer(i);
    return Value::map(m);
}

static std::set<std::string> primaryKeySet(const std::vector<QueryResult>& results) {
    std::set<std::string> out;
    for (const auto& r : results) out.insert(std::string(toStringView(toByteView(r.primaryKey))));
    return out;
}

int main() {
    constexpr int kN = 40;

    // ================= scan.apdb: TableScan baseline (no index) =================
    std::set<std::string> eqCat2, prefixCat, geCat3, ltCat3, leCat3, gtCat3, neCat2, existsAll;
    {
        auto path = anntest::scratchDbPath("test_query_scan.apdb");
        auto envR = Environment::open(path);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        auto codecR = createMsgPackCodec();
        CHECK(codecR.ok());
        auto codec = codecR.moveValue();
        DocumentStore store(*codec);
        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            for (int i = 0; i < kN; ++i) {
                CHECK_OK(store.put(wtxn, toByteView(keyFor(i)), docFor(i)));
            }
            CHECK_OK(wtxn.commit());
        }

        QueryEngine scanEngine(*codec, nullptr);
        auto rtxnR = env.beginRead();
        CHECK(rtxnR.ok());
        txn::ReadTransaction rtxn = rtxnR.moveValue();

        CHECK(scanEngine.explain(Query{{{"category", Op::Eq, "cat2"}}, 0, {}, 1}).path ==
              AccessPath::TableScan);

        auto run = [&](Predicate p) -> std::set<std::string> {
            Query q; q.predicates = {p};
            auto r = scanEngine.execute(rtxn, q);
            CHECK(r.ok());
            return r.ok() ? primaryKeySet(r.value()) : std::set<std::string>{};
        };

        eqCat2 = run({"category", Op::Eq, "cat2"});
        CHECK(eqCat2.size() == 8);
        neCat2 = run({"category", Op::Ne, "cat2"});
        CHECK(neCat2.size() == 32);
        prefixCat = run({"category", Op::Prefix, "cat"});
        CHECK(prefixCat.size() == 40);
        existsAll = run({"category", Op::Exists, ""});
        CHECK(existsAll.size() == 40);
        ltCat3 = run({"category", Op::Lt, "cat3"});
        CHECK(ltCat3.size() == 24);
        leCat3 = run({"category", Op::Le, "cat3"});
        CHECK(leCat3.size() == 32);
        gtCat3 = run({"category", Op::Gt, "cat3"});
        CHECK(gtCat3.size() == 8);
        geCat3 = run({"category", Op::Ge, "cat3"});
        CHECK(geCat3.size() == 16);

        // Double-check filtering isn't accidentally right-by-coincidence:
        // decode every Eq "cat2" result and confirm the field really matches.
        {
            Query q; q.predicates = {{"category", Op::Eq, "cat2"}};
            auto r = scanEngine.execute(rtxn, q);
            CHECK(r.ok());
            if (r.ok()) {
                for (auto& row : r.value()) {
                    Value v;
                    CHECK_OK(codec->decodeValue(row.doc.raw(), v));
                    CHECK(v.asMap().at("category").asString() == "cat2");
                }
            }
        }

        // Numeric-vs-lexical predicate-comparison quirk: Predicate::value is
        // always a plain string and compareValues() compares as strings, so
        // "score >= 9" over the integers 0..39 does NOT mean ">= 9
        // numerically" -- lexically, every two-digit string starting with
        // '1'/'2'/'3' sorts BEFORE the single character "9" ('1' < '9', ...,
        // '3' < '9'), so only the single doc whose score is *exactly* 9
        // matches, not the 30 docs a numeric reading would expect.
        {
            Query q; q.predicates = {{"score", Op::Ge, "9"}};
            auto r = scanEngine.execute(rtxn, q);
            CHECK(r.ok());
            if (r.ok()) {
                CHECK(r.value().size() == 1);
                if (r.value().size() == 1) CHECK(toStringView(toByteView(r.value()[0].primaryKey)) == keyFor(9));
            }
        }

        // limit truncates the result count.
        {
            Query q; q.predicates = {{"category", Op::Exists, ""}}; q.limit = 5;
            auto r = scanEngine.execute(rtxn, q);
            CHECK(r.ok());
            if (r.ok()) CHECK(r.value().size() == 5);
        }

        // project returns only the requested field, doc left empty.
        {
            Query q; q.predicates = {{"category", Op::Eq, "cat2"}}; q.project = {"category"};
            auto r = scanEngine.execute(rtxn, q);
            CHECK(r.ok());
            if (r.ok()) {
                for (auto& row : r.value()) {
                    CHECK(row.doc.raw().empty());
                    CHECK(row.projected.size() == 1);
                    if (row.projected.size() == 1) {
                        CHECK(row.projected[0].path == "category");
                        CHECK(row.projected[0].value == "cat2");
                    }
                }
            }
        }

        // countEstimate matches execute(...).size() for a representative query.
        {
            Query q; q.predicates = {{"category", Op::Prefix, "cat"}};
            auto execR = scanEngine.execute(rtxn, q);
            auto cntR = scanEngine.countEstimate(rtxn, q);
            CHECK(execR.ok() && cntR.ok());
            if (execR.ok() && cntR.ok()) CHECK(cntR.value() == execR.value().size());
        }

        // parallelWorkers=1 vs 4 over >=32 candidates return the same result set.
        {
            Query q1; q1.predicates = {{"category", Op::Exists, ""}}; q1.parallelWorkers = 1;
            Query q4 = q1; q4.parallelWorkers = 4;
            auto r1 = scanEngine.execute(rtxn, q1);
            auto r4 = scanEngine.execute(rtxn, q4);
            CHECK(r1.ok() && r4.ok());
            if (r1.ok() && r4.ok()) CHECK(primaryKeySet(r1.value()) == primaryKeySet(r4.value()));
        }

        CHECK_OK(env.close());
    }

    // ================= idx.apdb: index-backed access paths =================
    {
        auto path = anntest::scratchDbPath("test_query_idx.apdb");
        auto envR = Environment::open(path);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        auto codecR = createMsgPackCodec();
        CHECK(codecR.ok());
        auto codec = codecR.moveValue();

        btree::BTree tree(env.pageManager());
        IndexManager indexes(tree, env.pageManager());
        {
            auto rtxnR = env.beginRead();
            txn::ReadTransaction rtxn = rtxnR.moveValue();
            CHECK_OK(indexes.loadCatalog(rtxn));
        }
        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            CHECK_OK(indexes.createIndex("by_category", "category", wtxn));
            CHECK_OK(indexes.saveCatalog(wtxn));
            CHECK_OK(wtxn.commit());
        }

        // Seed the identical dataset *after* the index exists, through a
        // DocumentStore with the index attached, so every put is maintained.
        DocumentStore store(*codec, &indexes);
        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            for (int i = 0; i < kN; ++i) {
                CHECK_OK(store.put(wtxn, toByteView(keyFor(i)), docFor(i)));
            }
            CHECK_OK(wtxn.commit());
        }

        QueryEngine idxEngine(*codec, &indexes);
        auto rtxnR = env.beginRead();
        CHECK(rtxnR.ok());
        txn::ReadTransaction rtxn = rtxnR.moveValue();

        {
            QueryPlan p = idxEngine.explain(Query{{{"category", Op::Eq, "cat2"}}, 0, {}, 1});
            CHECK(p.path == AccessPath::IndexPoint);
            CHECK(p.indexName == "by_category");
        }
        {
            QueryPlan p = idxEngine.explain(Query{{{"category", Op::Prefix, "cat"}}, 0, {}, 1});
            CHECK(p.path == AccessPath::IndexPrefix);
        }
        {
            QueryPlan p = idxEngine.explain(Query{{{"category", Op::Ge, "cat3"}}, 0, {}, 1});
            CHECK(p.path == AccessPath::IndexRange);
        }

        // The index-driven access paths must return the identical logical
        // result set as the unindexed table scan of the same data -- the
        // planner's choice must never change correctness.
        auto runIdx = [&](Predicate p) -> std::set<std::string> {
            Query q; q.predicates = {p};
            auto r = idxEngine.execute(rtxn, q);
            CHECK(r.ok());
            return r.ok() ? primaryKeySet(r.value()) : std::set<std::string>{};
        };
        CHECK(runIdx({"category", Op::Eq, "cat2"}) == eqCat2);
        CHECK(runIdx({"category", Op::Prefix, "cat"}) == prefixCat);
        CHECK(runIdx({"category", Op::Ge, "cat3"}) == geCat3);
        CHECK(runIdx({"category", Op::Lt, "cat3"}) == ltCat3);
        CHECK(runIdx({"category", Op::Le, "cat3"}) == leCat3);
        CHECK(runIdx({"category", Op::Gt, "cat3"}) == gtCat3);

        // countEstimate's cheap single-predicate index path.
        {
            Query q; q.predicates = {{"category", Op::Eq, "cat2"}};
            auto execR = idxEngine.execute(rtxn, q);
            auto cntR = idxEngine.countEstimate(rtxn, q);
            CHECK(execR.ok() && cntR.ok());
            if (execR.ok() && cntR.ok()) CHECK(cntR.value() == execR.value().size());
        }

        CHECK_OK(env.close());
    }

    TEST_MAIN_END();
}
