// LMDB-parity extras on Cursor/WriteTransaction (final inventory pass):
// count(), replace(), pop(), getMulti(), putMulti(), seekRangeDup().
#include "test_common.hpp"

#include <cstring>
#include <string>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/btree/cursor.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;
using namespace annpocketdb::btree;

int main() {
    auto path = anntest::scratchDbPath("cursor_extras.apdb");
    auto envR = Environment::open(path);
    CHECK(envR.ok());
    if (!envR) TEST_MAIN_END();
    Environment env = envR.moveValue();

    // ---- count(): non-dup key, flat DUPSORT, DUPFIXED run, escalated sub-tree ----
    {
        auto wtxnR = env.beginWrite();
        CHECK(wtxnR.ok());
        if (!wtxnR) TEST_MAIN_END();
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        CHECK_OK(wtxn.put(toByteView("unique_key"), toByteView("v")));

        Cursor dupCur = wtxn.openCursor();
        dupCur.setFlags(DbFlags::DupSort);
        CHECK_OK(dupCur.put(toByteView("dup_key"), toByteView("a"), CursorPutFlags::AppendDup));
        CHECK_OK(dupCur.put(toByteView("dup_key"), toByteView("b"), CursorPutFlags::AppendDup));
        CHECK_OK(dupCur.put(toByteView("dup_key"), toByteView("c"), CursorPutFlags::AppendDup));

        Cursor fixedCur = wtxn.openCursor();
        fixedCur.setFlags(DbFlags::DupFixed);
        std::uint32_t elems[4] = {10, 20, 30, 40};
        ByteBuffer blob(sizeof(elems));
        std::memcpy(blob.data(), elems, sizeof(elems));
        CHECK_OK(fixedCur.putMultiple(toByteView("fixed_key"), sizeof(std::uint32_t), toByteView(blob)));

        // Escalated sub-tree: enough dups to cross the escalation threshold,
        // bounded by sentinels so the group never touches a leaf edge.
        CHECK_OK(wtxn.put(toByteView("aaa_sentinel"), toByteView("s")));
        CHECK_OK(wtxn.put(toByteView("zzz_sentinel"), toByteView("s")));
        Cursor escCur = wtxn.openCursor();
        escCur.setFlags(DbFlags::DupSort);
        std::string pad(20, 'v');
        for (int i = 0; i < 150; ++i) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%04d", i);
            std::string v = std::string(buf) + pad;
            CHECK_OK(escCur.put(toByteView("mmm_dupkey"), toByteView(v), CursorPutFlags::AppendDup));
        }
        CHECK_OK(wtxn.commit());
    }
    {
        auto rtxnR = env.beginRead();
        CHECK(rtxnR.ok());
        if (rtxnR.ok()) {
            txn::ReadTransaction rtxn = rtxnR.moveValue();

            Cursor cur = rtxn.openCursor();
            CHECK(cur.seekExact(toByteView("unique_key")).value());
            auto c1 = cur.count();
            CHECK(c1.ok() && c1.value() == 1);

            Cursor dupCur = rtxn.openCursor();
            dupCur.setFlags(DbFlags::DupSort);
            CHECK(dupCur.seekExact(toByteView("dup_key")).value());
            auto c2 = dupCur.count();
            CHECK(c2.ok() && c2.value() == 3);

            Cursor fixedCur = rtxn.openCursor();
            fixedCur.setFlags(DbFlags::DupFixed);
            CHECK(fixedCur.seekExact(toByteView("fixed_key")).value());
            auto c3 = fixedCur.count();
            CHECK(c3.ok() && c3.value() == 4);

            Cursor escCur = rtxn.openCursor();
            escCur.setFlags(DbFlags::DupSort);
            CHECK(escCur.seekExact(toByteView("mmm_dupkey")).value());
            auto c4 = escCur.count();
            CHECK(c4.ok() && c4.value() == 150);
        }
    }

    // ---- replace() / pop(): Cursor and WriteTransaction levels ----
    {
        auto wtxnR = env.beginWrite();
        CHECK(wtxnR.ok());
        if (!wtxnR) TEST_MAIN_END();
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        CHECK_OK(wtxn.put(toByteView("replace_key"), toByteView("original")));

        {
            Cursor cur = wtxn.openCursor();
            auto old = cur.replace(toByteView("replace_key"), toByteView("updated"));
            CHECK(old.ok());
            if (old.ok()) {
                CHECK(old.value().has_value());
                if (old.value().has_value()) {
                    CHECK(toStringView(toByteView(*old.value())) == "original");
                }
            }
        }
        auto v1 = wtxn.get(toByteView("replace_key"));
        CHECK(v1.ok());
        if (v1.ok()) CHECK(toStringView(toByteView(v1.value())) == "updated");

        // replace() on a brand-new key: nullopt returned, but the put happens.
        {
            Cursor cur = wtxn.openCursor();
            auto old = cur.replace(toByteView("brand_new_key"), toByteView("first_value"));
            CHECK(old.ok());
            if (old.ok()) CHECK(!old.value().has_value());
        }
        auto v2 = wtxn.get(toByteView("brand_new_key"));
        CHECK(v2.ok());
        if (v2.ok()) CHECK(toStringView(toByteView(v2.value())) == "first_value");

        // Transaction-level replace()/pop() convenience.
        auto txnOld = wtxn.replace(toByteView("replace_key"), toByteView("via_txn"));
        CHECK(txnOld.ok());
        if (txnOld.ok() && txnOld.value().has_value()) {
            CHECK(toStringView(toByteView(*txnOld.value())) == "updated");
        }

        auto popped = wtxn.pop(toByteView("replace_key"));
        CHECK(popped.ok());
        if (popped.ok()) {
            CHECK(popped.value().has_value());
            if (popped.value().has_value()) CHECK(toStringView(toByteView(*popped.value())) == "via_txn");
        }
        CHECK(!wtxn.contains(toByteView("replace_key")).value());

        // pop() on a missing key: nullopt, no error.
        auto missPop = wtxn.pop(toByteView("does_not_exist"));
        CHECK(missPop.ok());
        if (missPop.ok()) CHECK(!missPop.value().has_value());

        CHECK_OK(wtxn.commit());
    }

    // ---- getMulti() / putMulti() ----
    {
        auto wtxnR = env.beginWrite();
        CHECK(wtxnR.ok());
        if (!wtxnR) TEST_MAIN_END();
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        Cursor cur = wtxn.openCursor();

        std::vector<std::pair<ByteView, ByteView>> items{
            {toByteView("batch_c"), toByteView("3")},
            {toByteView("batch_a"), toByteView("1")},
            {toByteView("batch_b"), toByteView("2")},
        };
        auto put1 = cur.putMulti(items, /*overwrite=*/true, /*append=*/false);
        CHECK(put1.ok());
        if (put1.ok()) {
            CHECK(put1.value().consumed == 3);
            CHECK(put1.value().added == 3);
        }

        // overwrite=false against an existing key: consumed but not added.
        std::vector<std::pair<ByteView, ByteView>> collide{{toByteView("batch_a"), toByteView("clobber")}};
        auto put2 = cur.putMulti(collide, /*overwrite=*/false, /*append=*/false);
        CHECK(put2.ok());
        if (put2.ok()) {
            CHECK(put2.value().consumed == 1);
            CHECK(put2.value().added == 0);
        }
        auto stillOrig = wtxn.get(toByteView("batch_a"));
        CHECK(stillOrig.ok());
        if (stillOrig.ok()) CHECK(toStringView(toByteView(stillOrig.value())) == "1");

        // append=true with correctly-ascending NEW keys, sorted after
        // everything currently in the tree (including "zzz_sentinel" from
        // the count() setup above -- "zzzz_" sorts after "zzz_" since '_' <
        // 'z' at the differing position).
        std::vector<std::pair<ByteView, ByteView>> appended{
            {toByteView("zzzz_append_1"), toByteView("x")},
            {toByteView("zzzz_append_2"), toByteView("y")},
        };
        auto put3 = cur.putMulti(appended, /*overwrite=*/true, /*append=*/true);
        CHECK(put3.ok());
        if (put3.ok()) CHECK(put3.value().added == 2);

        // append=true where the first pair is a valid append but the second
        // is not (sorts before the first, not after) must fail the whole
        // call rather than corrupt the tree.
        std::vector<std::pair<ByteView, ByteView>> badOrder{
            {toByteView("zzzzz_append_3"), toByteView("x")},
            {toByteView("aaa_out_of_order"), toByteView("y")},
        };
        auto put4 = cur.putMulti(badOrder, /*overwrite=*/true, /*append=*/true);
        CHECK(!put4.ok());

        CHECK_OK(wtxn.commit());
    }
    {
        auto rtxnR = env.beginRead();
        CHECK(rtxnR.ok());
        if (rtxnR.ok()) {
            txn::ReadTransaction rtxn = rtxnR.moveValue();
            Cursor cur = rtxn.openCursor();
            std::vector<ByteView> keys{toByteView("batch_a"), toByteView("nope_missing"), toByteView("batch_c")};
            auto results = cur.getMulti(keys);
            CHECK(results.ok());
            if (results.ok()) {
                CHECK(results.value().size() == 2);  // missing key silently skipped
                CHECK(toStringView(toByteView(results.value()[0].second)) == "1");
                CHECK(toStringView(toByteView(results.value()[1].second)) == "3");
            }

            // getMulti with dupData=true over a DUPSORT key.
            Cursor dupCur = rtxn.openCursor();
            dupCur.setFlags(DbFlags::DupSort);
            std::vector<ByteView> dupKeys{toByteView("dup_key")};
            auto dupResults = dupCur.getMulti(dupKeys, /*dupData=*/true);
            CHECK(dupResults.ok());
            if (dupResults.ok()) CHECK(dupResults.value().size() == 3);
        }
    }

    // ---- seekRangeDup() ----
    {
        auto rtxnR = env.beginRead();
        CHECK(rtxnR.ok());
        if (rtxnR.ok()) {
            txn::ReadTransaction rtxn = rtxnR.moveValue();
            Cursor cur = rtxn.openCursor();
            cur.setFlags(DbFlags::DupSort);

            // dup_key holds "a", "b", "c" (sorted). Seek for first >= "b".
            auto r1 = cur.seekRangeDup(toByteView("dup_key"), toByteView("b"));
            CHECK(r1.ok() && r1.value());
            if (r1.ok() && r1.value()) {
                auto v = cur.valueView();
                CHECK(v.ok() && toStringView(v.value()) == "b");
            }

            // First value >= "aa" (between "a" and "b") should land on "b".
            auto r2 = cur.seekRangeDup(toByteView("dup_key"), toByteView("aa"));
            CHECK(r2.ok() && r2.value());
            if (r2.ok() && r2.value()) {
                auto v = cur.valueView();
                CHECK(v.ok() && toStringView(v.value()) == "b");
            }

            // Nothing >= "z" for this key: ok status, false (not NotFound).
            auto r3 = cur.seekRangeDup(toByteView("dup_key"), toByteView("z"));
            CHECK(r3.ok() && !r3.value());

            // A key that doesn't exist at all: NotFound.
            auto r4 = cur.seekRangeDup(toByteView("no_such_key"), toByteView("x"));
            CHECK(!r4.ok());
        }
    }

    CHECK_OK(env.close());
    TEST_MAIN_END();
}
