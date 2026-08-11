// MVCC / snapshot isolation: a ReadTransaction's view must not change no
// matter what commits after it -- including many subsequent commits to the
// exact same key, and commits that force real COW leaf/branch splits deep
// in the tree while the snapshot is still open.
#include "test_common.hpp"

#include <string>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/btree/cursor.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;

namespace {
std::string keyFor(int i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "key%04d", i);
    return std::string(buf);
}
std::string valFor(int i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "val%04d", i);
    return std::string(buf);
}
// Counts entries visible through a transaction's cursor (works for both
// ReadTransaction and WriteTransaction since both expose openCursor()).
template <typename Txn>
int countEntries(Txn& txn) {
    btree::Cursor cur = txn.openCursor();
    auto first = cur.first();
    if (!first.ok() || !first.value()) return 0;
    int n = 1;
    while (true) {
        auto next = cur.next();
        if (!next.ok() || !next.value()) break;
        ++n;
    }
    return n;
}
}  // namespace

int main() {
    auto path = anntest::scratchDbPath("mvcc.apdb");

    auto envR = Environment::open(path);
    CHECK(envR.ok());
    if (!envR) TEST_MAIN_END();
    Environment env = envR.moveValue();

    // ---- Baseline: single key, single commit ----
    CHECK_OK(env.put(toByteView("k"), toByteView("v1")));

    auto rtxnR = env.beginRead();
    CHECK(rtxnR.ok());
    if (!rtxnR) TEST_MAIN_END();
    txn::ReadTransaction rtxn = rtxnR.moveValue();

    auto before = rtxn.get(toByteView("k"));
    CHECK(before.ok());
    if (before.ok()) CHECK(toStringView(toByteView(before.value())) == "v1");

    CHECK_OK(env.put(toByteView("k"), toByteView("v2")));

    auto stillV1 = rtxn.get(toByteView("k"));
    CHECK(stillV1.ok());
    if (stillV1.ok()) CHECK(toStringView(toByteView(stillV1.value())) == "v1");

    auto rtxn2R = env.beginRead();
    CHECK(rtxn2R.ok());
    if (rtxn2R.ok()) {
        auto v = rtxn2R.value().get(toByteView("k"));
        CHECK(v.ok());
        if (v.ok()) CHECK(toStringView(toByteView(v.value())) == "v2");
    }

    // ---- An open ReadTransaction keeps seeing its original snapshot
    // through TEN subsequent commits to the same key ----
    {
        auto snapR = env.beginRead();
        CHECK(snapR.ok());
        if (snapR.ok()) {
            txn::ReadTransaction snap = snapR.moveValue();
            auto initial = snap.get(toByteView("k"));
            CHECK(initial.ok());
            std::string frozenValue;
            if (initial.ok()) frozenValue = std::string(toStringView(toByteView(initial.value())));
            CHECK(frozenValue == "v2");  // whatever the committed value was when the snapshot opened

            for (int commitNum = 0; commitNum < 10; ++commitNum) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "churn%02d", commitNum);
                CHECK_OK(env.put(toByteView("k"), toByteView(std::string(buf))));
                // The long-held snapshot must be unaffected by *this specific*
                // commit and by every one before it in this loop.
                auto stillFrozen = snap.get(toByteView("k"));
                CHECK(stillFrozen.ok());
                if (stillFrozen.ok()) {
                    CHECK(toStringView(toByteView(stillFrozen.value())) == frozenValue);
                }
            }

            // A fresh snapshot after all ten commits sees the last write.
            auto freshR = env.beginRead();
            CHECK(freshR.ok());
            if (freshR.ok()) {
                auto v = freshR.value().get(toByteView("k"));
                CHECK(v.ok());
                if (v.ok()) CHECK(toStringView(toByteView(v.value())) == "churn09");
            }
        }
    }

    // ---- Concurrent-reader-vs-split isolation: a snapshot opened while the
    // tree is small (one leaf) must remain completely unaffected by later
    // commits that force many real COW leaf/branch splits ----
    {
        // Small, single-leaf-sized initial population.
        constexpr int kInitial = 50;
        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            if (!wtxnR) TEST_MAIN_END();
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            for (int i = 0; i < kInitial; ++i) {
                CHECK_OK(wtxn.put(toByteView(keyFor(i)), toByteView(valFor(i))));
            }
            CHECK_OK(wtxn.commit());
        }

        auto splitSnapR = env.beginRead();
        CHECK(splitSnapR.ok());
        if (!splitSnapR) TEST_MAIN_END();
        txn::ReadTransaction splitSnap = splitSnapR.moveValue();

        // Snapshot sees exactly the initial population (plus the unrelated
        // "k" key from earlier in this test).
        CHECK(countEntries(splitSnap) == kInitial + 1);
        for (int i = 0; i < kInitial; ++i) {
            auto v = splitSnap.get(toByteView(keyFor(i)));
            CHECK(v.ok());
            if (v.ok()) CHECK(toStringView(toByteView(v.value())) == valFor(i));
        }

        // Now force many real leaf/branch splits with the snapshot still open.
        constexpr int kGrown = 3000;
        {
            auto wtxnR = env.beginWrite();
            CHECK(wtxnR.ok());
            if (!wtxnR) TEST_MAIN_END();
            txn::WriteTransaction wtxn = wtxnR.moveValue();
            for (int i = kInitial; i < kGrown; ++i) {
                CHECK_OK(wtxn.put(toByteView(keyFor(i)), toByteView(valFor(i))));
            }
            CHECK_OK(wtxn.commit());
        }

        // The old snapshot must be completely untouched by the splits: same
        // entry count, same values, and keys that didn't exist yet at
        // snapshot time are still invisible to it.
        CHECK(countEntries(splitSnap) == kInitial + 1);
        for (int i = 0; i < kInitial; ++i) {
            auto v = splitSnap.get(toByteView(keyFor(i)));
            CHECK(v.ok());
            if (v.ok()) CHECK(toStringView(toByteView(v.value())) == valFor(i));
        }
        auto shouldBeMissing = splitSnap.get(toByteView(keyFor(kInitial + 5)));
        CHECK(!shouldBeMissing.ok());
        CHECK(shouldBeMissing.status().code() == StatusCode::NotFound);

        // A brand-new snapshot after the growth sees everything.
        auto grownR = env.beginRead();
        CHECK(grownR.ok());
        if (grownR.ok()) {
            txn::ReadTransaction grown = grownR.moveValue();
            CHECK(countEntries(grown) == kGrown + 1);
            auto v = grown.get(toByteView(keyFor(kInitial + 5)));
            CHECK(v.ok());
            if (v.ok()) CHECK(toStringView(toByteView(v.value())) == valFor(kInitial + 5));
        }
    }

    CHECK_OK(env.close());
    TEST_MAIN_END();
}
