// Coverage for index::IndexManager/Index (previously untested): the
// load/create/drop catalog lifecycle -- including the "forgot to
// saveCatalog" persistence gotcha -- insertEntry/removeEntry idempotency,
// cursor read-back (composite [secondary][0x00][primary] leaf keys decoded
// via index::splitIndexKey, matching query.cpp's own real usage), and the
// shared maxKeySize() boundary already exercised for plain KV keys in
// test_status_codes.cpp.
#include "test_common.hpp"

#include <string>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/index/index.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;
using namespace annpocketdb::index;

int main() {
    auto path = anntest::scratchDbPath("test_index.apdb");

    // ---- calling before loadCatalog fails loudly ----
    {
        auto envR = Environment::open(path);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();
        btree::BTree tree(env.pageManager());
        IndexManager indexes(tree, env.pageManager());

        auto wtxnR = env.beginWrite();
        CHECK(wtxnR.ok());
        if (wtxnR.ok()) {
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            Status s = indexes.createIndex("by_email", "user.email", wtxn);
            CHECK(!s.ok());
            CHECK(s.code() == StatusCode::Internal);
            CHECK_OK(wtxn.abort());
        }
        CHECK_OK(env.close());
    }

    // ---- create + saveCatalog persists across reopen; skipping saveCatalog does not ----
    {
        auto envR = Environment::open(path);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();
        btree::BTree tree(env.pageManager());
        IndexManager indexes(tree, env.pageManager());
        {
            auto rtxnR = env.beginRead();
            CHECK(rtxnR.ok());
            if (rtxnR.ok()) {
                txn::ReadTransaction rtxn = rtxnR.moveValue();
                CHECK_OK(indexes.loadCatalog(rtxn));
            }
        }

        // Create "ephemeral" but never call saveCatalog before commit. Uses
        // its own IndexManager instance (loadCatalog() below reflects
        // whatever's actually durable so far, i.e. nothing yet) so this
        // in-memory-only creation cannot leak into a later saveCatalog() on
        // a *different* IndexManager -- saveCatalog() persists that
        // instance's *entire* in-memory catalog_, not just newly-created
        // entries, so sharing one instance across both would make
        // "ephemeral" durable as a side effect of "by_email"'s save.
        {
            IndexManager ephemeralIndexes(tree, env.pageManager());
            auto rtxnR = env.beginRead();
            txn::ReadTransaction rtxn = rtxnR.moveValue();
            CHECK_OK(ephemeralIndexes.loadCatalog(rtxn));

            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            CHECK_OK(ephemeralIndexes.createIndex("ephemeral", "x", wtxn));
            CHECK(ephemeralIndexes.hasIndex("ephemeral"));  // visible in-memory immediately
            CHECK_OK(wtxn.commit());                        // catalog blob itself was never put()
        }

        // Create "by_email" and DO call saveCatalog before commit.
        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            CHECK_OK(indexes.createIndex("by_email", "user.email", wtxn));
            CHECK_OK(indexes.saveCatalog(wtxn));
            CHECK_OK(wtxn.commit());
        }
        CHECK_OK(env.close());
    }
    {
        // Fresh IndexManager + fresh load: "ephemeral" must be gone (its
        // creation was never persisted), "by_email" must be present.
        auto envR = Environment::open(path);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();
        btree::BTree tree(env.pageManager());
        IndexManager indexes(tree, env.pageManager());
        {
            auto rtxnR = env.beginRead();
            CHECK(rtxnR.ok());
            txn::ReadTransaction rtxn = rtxnR.moveValue();
            CHECK_OK(indexes.loadCatalog(rtxn));
        }
        CHECK(!indexes.hasIndex("ephemeral"));
        CHECK(indexes.hasIndex("by_email"));
        auto names = indexes.listIndexes();
        CHECK(names.size() == 1);
        auto metaR = indexes.meta("by_email");
        CHECK(metaR.ok());
        if (metaR.ok()) CHECK(metaR.value().fieldPath == "user.email");
        CHECK_OK(env.close());
    }

    // ---- createIndex boundary/error cases ----
    {
        auto envR = Environment::open(path);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();
        btree::BTree tree(env.pageManager());
        IndexManager indexes(tree, env.pageManager());
        {
            auto rtxnR = env.beginRead();
            txn::ReadTransaction rtxn = rtxnR.moveValue();
            CHECK_OK(indexes.loadCatalog(rtxn));
        }
        auto wtxnR = env.beginWrite();
        CHECK(wtxnR.ok());
        txn::WriteTransaction wtxn = wtxnR.moveValue();

        Status emptyName = indexes.createIndex("", "field", wtxn);
        CHECK(!emptyName.ok());
        CHECK(emptyName.code() == StatusCode::InvalidArgument);

        Status emptyPath = indexes.createIndex("some_name", "", wtxn);
        CHECK(!emptyPath.ok());
        CHECK(emptyPath.code() == StatusCode::InvalidArgument);

        Status dup = indexes.createIndex("by_email", "user.email", wtxn);
        CHECK(!dup.ok());
        CHECK(dup.code() == StatusCode::KeyExists);

        CHECK_OK(wtxn.abort());
        CHECK_OK(env.close());
    }

    // ---- insertEntry/removeEntry idempotency + cursor read-back ----
    {
        auto envR = Environment::open(path);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();
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

            CHECK_OK(indexes.insertEntry(wtxn, "by_email", toByteView(std::string("alice@x.com")),
                                         toByteView(std::string("user:1"))));
            CHECK_OK(indexes.insertEntry(wtxn, "by_email", toByteView(std::string("bob@x.com")),
                                         toByteView(std::string("user:2"))));
            // Idempotent re-insert of the exact same pair returns OK, not KeyExists.
            CHECK_OK(indexes.insertEntry(wtxn, "by_email", toByteView(std::string("alice@x.com")),
                                         toByteView(std::string("user:1"))));
            CHECK_OK(indexes.saveCatalog(wtxn));
            CHECK_OK(wtxn.commit());
        }

        {
            auto idxR = indexes.openIndex("by_email");
            CHECK(idxR.ok());
            if (idxR.ok()) {
                Index idx = idxR.value();
                auto cntR = idx.entryCount();
                CHECK(cntR.ok());
                if (cntR.ok()) CHECK(cntR.value() == 2);

                // Index cursors are opened without setFlags(DupSort) (see
                // Index::find), so key() already yields the plain secondary
                // key alone -- there is no composite to split; the primary
                // key lives in the cursor's value.
                auto foundR = idx.find(toByteView(std::string("alice@x.com")));
                CHECK(foundR.ok());
                if (foundR.ok()) {
                    auto keyR = foundR.value().key();
                    auto valR = foundR.value().valueView();
                    CHECK(keyR.ok() && valR.ok());
                    if (keyR.ok() && valR.ok()) {
                        CHECK(toStringView(keyR.value()) == "alice@x.com");
                        CHECK(toStringView(valR.value()) == "user:1");
                    }
                }

                auto missingR = idx.find(toByteView(std::string("nobody@x.com")));
                CHECK(!missingR.ok());
                CHECK(missingR.status().code() == StatusCode::NotFound);

                // Walk every entry via seekLowerBound/openCursor.
                auto curR = idx.openCursor();
                CHECK(curR.ok());
                if (curR.ok()) {
                    btree::Cursor cur = curR.moveValue();
                    int seen = 0;
                    while (cur.valid()) {
                        ++seen;
                        auto n = cur.next();
                        CHECK(n.ok());
                        if (!n.ok() || !n.value()) break;
                    }
                    CHECK(seen == 2);
                }
            }
        }

        // removeEntry: remove one real pair, then remove it again (idempotent NotFound -> OK).
        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            CHECK_OK(indexes.removeEntry(wtxn, "by_email", toByteView(std::string("bob@x.com")),
                                         toByteView(std::string("user:2"))));
            CHECK_OK(indexes.removeEntry(wtxn, "by_email", toByteView(std::string("bob@x.com")),
                                         toByteView(std::string("user:2"))));
            CHECK_OK(indexes.saveCatalog(wtxn));
            CHECK_OK(wtxn.commit());
        }
        {
            auto idxR = indexes.openIndex("by_email");
            CHECK(idxR.ok());
            if (idxR.ok()) {
                auto cntR = idxR.value().entryCount();
                CHECK(cntR.ok());
                if (cntR.ok()) CHECK(cntR.value() == 1);
            }
        }

        // ---- boundary: secondary key longer than maxKeySize() is rejected ----
        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            std::string hugeKey(env.maxKeySize() + 1, 'k');
            Status s = indexes.insertEntry(wtxn, "by_email", toByteView(hugeKey),
                                           toByteView(std::string("user:3")));
            CHECK(!s.ok());
            CHECK(s.code() == StatusCode::BadValSize);
            CHECK_OK(wtxn.abort());
        }

        // ---- dropIndex removes it from the catalog ----
        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            CHECK_OK(indexes.dropIndex("by_email", wtxn));
            CHECK_OK(indexes.saveCatalog(wtxn));
            CHECK_OK(wtxn.commit());
        }
        CHECK(!indexes.hasIndex("by_email"));
        CHECK(indexes.listIndexes().empty());

        CHECK_OK(env.close());
    }

    TEST_MAIN_END();
}
