// Nested transaction + cursor-generation-safety smoke test: a write-bound
// cursor opened before a nested transaction aborts must fail cleanly on its
// next use instead of silently reading rolled-back state.
#include "test_common.hpp"

#include "annpocketdb/api/environment.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;

int main() {
    auto path = anntest::scratchDbPath("nested_txn.apdb");

    auto envR = Environment::open(path);
    CHECK(envR.ok());
    if (!envR) TEST_MAIN_END();
    Environment env = envR.moveValue();

    auto wtxnR = env.beginWrite();
    CHECK(wtxnR.ok());
    if (!wtxnR) TEST_MAIN_END();
    txn::WriteTransaction wtxn = wtxnR.moveValue();

    CHECK_OK(wtxn.put(toByteView("outer"), toByteView("v")));

    btree::Cursor cur = wtxn.openCursor();
    auto seek = cur.seekExact(toByteView("outer"));
    CHECK(seek.ok() && seek.value());

    CHECK_OK(wtxn.beginNested());
    CHECK_OK(wtxn.put(toByteView("nested"), toByteView("should-vanish")));
    CHECK_OK(wtxn.abortNested());

    // The cursor predates (and survived) the nested scope; per design it is
    // conservatively invalidated by ANY nested abort, matching generation_'s
    // existing blunt move/destroy invalidation semantics.
    auto afterAbort = cur.next();
    CHECK(!afterAbort.ok());

    // Outer transaction still commits cleanly; only "outer" should exist.
    CHECK_OK(wtxn.commit());

    auto v1 = env.get(toByteView("outer"));
    CHECK(v1.ok());
    auto v2 = env.get(toByteView("nested"));
    CHECK(!v2.ok());
    CHECK(v2.status().code() == StatusCode::NotFound);

    CHECK_OK(env.close());
    TEST_MAIN_END();
}
