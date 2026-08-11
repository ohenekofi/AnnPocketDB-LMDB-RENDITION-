// DUPSORT sub-tree escalation smoke test: once one key's dup-set grows past
// roughly half a page, it's converted from flat per-value leaf cells into an
// embedded sub-tree (see BTree::escalateDupGroupIfNeeded). This must stay
// completely transparent to getAll()/cursor dup-navigation, support further
// inserts/removals after escalating, and correctly vanish once every value
// under that key is removed.
#include "test_common.hpp"

#include <algorithm>
#include <set>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/btree/cursor.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;
using namespace annpocketdb::btree;

static std::string padded(int i) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04d", i);
    return std::string(buf);
}

int main() {
    auto path = anntest::scratchDbPath("subtree_escalation.apdb");

    auto envR = Environment::open(path);
    CHECK(envR.ok());
    if (!envR) TEST_MAIN_END();
    Environment env = envR.moveValue();

    // Sentinels on either side of the escalating key so its dup-group never
    // touches the edge of whatever leaf it lives in.
    CHECK_OK(env.put(toByteView("aaa_sentinel"), toByteView("before")));
    CHECK_OK(env.put(toByteView("zzz_sentinel"), toByteView("after")));

    constexpr int kDupCount = 150;  // ~150 * (13+3+25) ~= 6150 bytes -- well past
                                     // the ~half-page escalation threshold.
    std::string valuePad(20, 'v');
    {
        auto wtxnR = env.beginWrite();
        CHECK(wtxnR.ok());
        if (!wtxnR) TEST_MAIN_END();
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        for (int i = 0; i < kDupCount; ++i) {
            std::string v = padded(i) + valuePad;
            Status s = wtxn.putDup(toByteView("mmm_dupkey"), toByteView(v));
            CHECK_OK(s);
        }
        CHECK_OK(wtxn.commit());
    }

    // getAll must return every value, and (since order.cmpValue is plain
    // byte compare here) sorted.
    {
        auto rtxnR = env.beginRead();
        CHECK(rtxnR.ok());
        if (rtxnR.ok()) {
            auto all = rtxnR.value().getAll(toByteView("mmm_dupkey"));
            CHECK(all.ok());
            if (all.ok()) {
                CHECK(all.value().size() == static_cast<std::size_t>(kDupCount));
                std::set<std::string> seen;
                std::string prev;
                bool sorted = true;
                for (auto& v : all.value()) {
                    std::string s(reinterpret_cast<const char*>(v.data()), v.size());
                    if (!prev.empty() && s < prev) sorted = false;
                    prev = s;
                    seen.insert(s);
                }
                CHECK(sorted);
                CHECK(seen.size() == static_cast<std::size_t>(kDupCount));
            }
        }
    }

    // Cursor dup-navigation over the (likely escalated) group.
    {
        auto rtxnR = env.beginRead();
        CHECK(rtxnR.ok());
        if (rtxnR.ok()) {
            txn::ReadTransaction rtxn = rtxnR.moveValue();
            btree::Cursor cur = rtxn.openCursor();
            auto seek = cur.seekExact(toByteView("mmm_dupkey"));
            CHECK(seek.ok() && seek.value());
            auto first = cur.firstDup();
            CHECK(first.ok() && first.value());
            int count = 1;
            for (;;) {
                auto n = cur.nextDup();
                if (!n.ok() || !n.value()) break;
                ++count;
            }
            CHECK(count == kDupCount);
            // Walk backward from the end too.
            auto last = cur.lastDup();
            CHECK(last.ok() && last.value());
            int backCount = 1;
            for (;;) {
                auto p = cur.prevDup();
                if (!p.ok() || !p.value()) break;
                ++backCount;
            }
            CHECK(backCount == kDupCount);
        }
    }

    // Sentinels must still be completely unaffected.
    auto before = env.get(toByteView("aaa_sentinel"));
    CHECK(before.ok());
    if (before.ok()) CHECK(toStringView(toByteView(before.value())) == "before");
    auto after = env.get(toByteView("zzz_sentinel"));
    CHECK(after.ok());
    if (after.ok()) CHECK(toStringView(toByteView(after.value())) == "after");

    // Remove one dup via removeDup, confirm count drops by exactly one.
    {
        std::string removedValue = padded(42) + valuePad;
        auto wtxnR = env.beginWrite();
        CHECK(wtxnR.ok());
        if (wtxnR.ok()) {
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            CHECK_OK(wtxn.removeDup(toByteView("mmm_dupkey"), toByteView(removedValue)));
            CHECK_OK(wtxn.commit());
        }
        auto all = env.beginRead();
        if (all.ok()) {
            auto vals = all.value().getAll(toByteView("mmm_dupkey"));
            CHECK(vals.ok());
            if (vals.ok()) CHECK(vals.value().size() == static_cast<std::size_t>(kDupCount - 1));
        }
    }

    // Remove the whole key; it (and its escalated sub-tree, if any) must
    // vanish completely, without disturbing the sentinels.
    {
        auto wtxnR = env.beginWrite();
        CHECK(wtxnR.ok());
        if (wtxnR.ok()) {
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            CHECK_OK(wtxn.remove(toByteView("mmm_dupkey")));
            CHECK_OK(wtxn.commit());
        }
        auto gone = env.get(toByteView("mmm_dupkey"));
        CHECK(!gone.ok());
        CHECK(gone.status().code() == StatusCode::NotFound);
        auto stillBefore = env.get(toByteView("aaa_sentinel"));
        CHECK(stillBefore.ok());
        auto stillAfter = env.get(toByteView("zzz_sentinel"));
        CHECK(stillAfter.ok());
    }

    CHECK_OK(env.close());
    TEST_MAIN_END();
}
