// Named sub-database smoke test: create/open/list/drop, cross-handle
// visibility, close()/clear() lifecycle, and staleness after close/drop.
#include "test_common.hpp"

#include "annpocketdb/api/environment.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;

int main() {
    auto path = anntest::scratchDbPath("subdatabase.apdb");

    auto envR = Environment::open(path);
    CHECK(envR.ok());
    if (!envR) TEST_MAIN_END();
    Environment env = envR.moveValue();

    auto sub1R = env.createSubDatabase("users");
    CHECK(sub1R.ok());
    if (!sub1R) TEST_MAIN_END();
    SubDatabase sub1 = sub1R.moveValue();

    CHECK_OK(sub1.put(toByteView("alice"), toByteView("1")));

    // A second, independently-opened handle to the SAME named database.
    auto sub2R = env.openSubDatabase("users");
    CHECK(sub2R.ok());
    if (!sub2R) TEST_MAIN_END();
    SubDatabase sub2 = sub2R.moveValue();

    auto aliceViaSub2 = sub2.get(toByteView("alice"));
    CHECK(aliceViaSub2.ok());

    // Write through sub2; sub1 must see it (proves root is resolved fresh
    // per transaction, not cached at handle-open time).
    CHECK_OK(sub2.put(toByteView("bob"), toByteView("2")));
    auto bobViaSub1 = sub1.get(toByteView("bob"));
    CHECK(bobViaSub1.ok());

    // Duplicate create is rejected.
    auto dupCreate = env.createSubDatabase("users");
    CHECK(!dupCreate.ok());
    CHECK(dupCreate.status().code() == StatusCode::KeyExists);

    // list includes it.
    auto names = env.listSubDatabases();
    CHECK(names.ok());
    if (names.ok()) {
        bool found = false;
        for (const auto& n : names.value()) found = found || (n == "users");
        CHECK(found);
    }

    // clear(): empties contents, handle stays valid.
    CHECK_OK(sub1.clear());
    auto afterClear = sub1.get(toByteView("alice"));
    CHECK(!afterClear.ok());
    CHECK_OK(sub1.put(toByteView("carol"), toByteView("3")));
    auto carol = sub2.get(toByteView("carol"));  // sub2 must see it too
    CHECK(carol.ok());

    // close(): sub1 and sub2 share the SAME underlying handle (open() cache-
    // hits create()'s handle for a name already opened in this process,
    // exactly like LMDB caching one MDB_dbi per name per process) -- so
    // closing it invalidates it for EVERY SubDatabase wrapper holding it,
    // not just the one that called close(). This is the documented contract
    // ("any SubDatabase still holding it will get BadDbi -- LMDB's
    // BadDbiError, 'the specified DBI was changed unexpectedly'"), not a bug.
    sub1.close();
    auto afterClose = sub1.get(toByteView("carol"));
    CHECK(!afterClose.ok());
    CHECK(afterClose.status().code() == StatusCode::BadDbi);
    auto sub2AfterClose = sub2.get(toByteView("carol"));
    CHECK(!sub2AfterClose.ok());
    CHECK(sub2AfterClose.status().code() == StatusCode::BadDbi);

    // The named database itself is untouched by close() -- a fresh open()
    // gets a new, independently-valid handle to the same data.
    auto sub3R = env.openSubDatabase("users");
    CHECK(sub3R.ok());
    if (sub3R.ok()) {
        SubDatabase sub3 = sub3R.moveValue();
        auto carolAgain = sub3.get(toByteView("carol"));
        CHECK(carolAgain.ok());

        // dropSubDatabase(): now sub3's handle must also go stale.
        CHECK_OK(env.dropSubDatabase("users"));
        auto sub3AfterDrop = sub3.get(toByteView("carol"));
        CHECK(!sub3AfterDrop.ok());
    }

    auto reopen = env.openSubDatabase("users");
    CHECK(!reopen.ok());
    CHECK(reopen.status().code() == StatusCode::NotFound);

    CHECK_OK(env.close());
    TEST_MAIN_END();
}
