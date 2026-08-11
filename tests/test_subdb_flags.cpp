// DbFlags persistence + custom comparator smoke test: a named database's
// flags (and a registered custom comparator) must survive a fresh open() by
// a different handle and actually govern storage order, not just a single
// cursor's own read-side navigation.
#include "test_common.hpp"

#include <algorithm>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/btree/cursor.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;
using namespace annpocketdb::btree;

int main() {
    auto path = anntest::scratchDbPath("subdb_flags.apdb");

    auto envR = Environment::open(path);
    CHECK(envR.ok());
    if (!envR) TEST_MAIN_END();
    Environment env = envR.moveValue();

    // ---- IntegerKey flag persists across a fresh open() ----
    {
        auto subR = env.createSubDatabase("ints", DbFlags::IntegerKey);
        CHECK(subR.ok());
        if (subR.ok()) {
            SubDatabase sub = subR.moveValue();
            std::uint32_t keys[3] = {300, 100, 200};
            for (std::uint32_t k : keys) {
                ByteBuffer kb(sizeof(k));
                std::memcpy(kb.data(), &k, sizeof(k));
                CHECK_OK(sub.put(toByteView(kb), toByteView("v")));
            }
        }
    }
    {
        // Fresh handle -- flags must have come back from the catalog record,
        // not from any in-process memory the first handle happened to hold.
        auto subR = env.openSubDatabase("ints");
        CHECK(subR.ok());
        if (subR.ok()) {
            SubDatabase sub = subR.moveValue();
            CHECK(sub.flags() == DbFlags::IntegerKey);
            std::uint32_t k = 200;
            ByteBuffer kb(sizeof(k));
            std::memcpy(kb.data(), &k, sizeof(k));
            auto v = sub.get(toByteView(kb));
            CHECK(v.ok());
        }
    }

    // ---- Custom comparator governs actual storage order ----
    {
        auto subR = env.createSubDatabase("custom");
        CHECK(subR.ok());
        if (!subR) TEST_MAIN_END();
        SubDatabase sub = subR.moveValue();

        // Reverse-lexicographic custom comparator.
        sub.manager()->setKeyComparator(sub.handle(), [](ByteView a, ByteView b) -> int {
            return -compareBytes(a, b);
        });

        CHECK_OK(sub.put(toByteView("alpha"), toByteView("1")));
        CHECK_OK(sub.put(toByteView("beta"), toByteView("2")));
        CHECK_OK(sub.put(toByteView("gamma"), toByteView("3")));

        auto wtxnR = env.beginWrite();
        CHECK(wtxnR.ok());
        if (wtxnR.ok()) {
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            wtxn.attachSubDbCatalog(sub.manager());
            btree::Cursor cur = wtxn.openCursorSubDb(sub.handle(), sub.openSeq());
            std::vector<std::string> order;
            auto f = cur.first();
            CHECK(f.ok());
            while (f.ok() && f.value()) {
                auto k = cur.key();
                CHECK(k.ok());
                if (k.ok()) order.emplace_back(reinterpret_cast<const char*>(k.value().data()), k.value().size());
                f = cur.next();
            }
            CHECK(order.size() == 3);
            if (order.size() == 3) {
                // Reverse-lexicographic: gamma, beta, alpha.
                CHECK(order[0] == "gamma");
                CHECK(order[1] == "beta");
                CHECK(order[2] == "alpha");
            }
            wtxn.abort();
        }
    }

    CHECK_OK(env.close());
    TEST_MAIN_END();
}
