// DUPSORT (ordinary sorted dup cells) and DUPFIXED (fixed-stride run via
// Cursor::putMultiple) smoke test.
#include "test_common.hpp"

#include <cstring>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/btree/db_flags.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;
using namespace annpocketdb::btree;

int main() {
    auto path = anntest::scratchDbPath("dupsort.apdb");

    auto envR = Environment::open(path);
    CHECK(envR.ok());
    if (!envR) TEST_MAIN_END();
    Environment env = envR.moveValue();

    // ---- Ordinary DUPSORT ----
    {
        auto wtxnR = env.beginWrite();
        CHECK(wtxnR.ok());
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        CHECK_OK(wtxn.putDup(toByteView("k"), toByteView("c")));
        CHECK_OK(wtxn.putDup(toByteView("k"), toByteView("a")));
        CHECK_OK(wtxn.putDup(toByteView("k"), toByteView("b")));
        CHECK_OK(wtxn.commit());
    }
    {
        auto rtxnR = env.beginRead();
        CHECK(rtxnR.ok());
        txn::ReadTransaction rtxn = rtxnR.moveValue();
        btree::Cursor cur = rtxn.openCursor();
        auto seek = cur.seekExact(toByteView("k"));
        CHECK(seek.ok() && seek.value());
        auto first = cur.firstDup();
        CHECK(first.ok() && first.value());
        std::string values[3];
        for (int i = 0; i < 3; ++i) {
            auto v = cur.valueView();
            CHECK(v.ok());
            if (v.ok()) values[i] = std::string(reinterpret_cast<const char*>(v.value().data()), v.value().size());
            if (i < 2) {
                auto n = cur.nextDup();
                CHECK(n.ok() && n.value());
            }
        }
        CHECK(values[0] == "a" && values[1] == "b" && values[2] == "c");
    }

    // ---- DUPFIXED via putMultiple ----
    {
        auto wtxnR = env.beginWrite();
        CHECK(wtxnR.ok());
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        btree::Cursor cur = wtxn.openCursor();
        cur.setFlags(DbFlags::DupFixed);

        constexpr std::uint32_t elemSize = 4;
        std::uint32_t elems[5] = {10, 20, 30, 40, 50};
        ByteBuffer blob(elemSize * 5);
        std::memcpy(blob.data(), elems, blob.size());
        CHECK_OK(cur.putMultiple(toByteView("fixed"), elemSize, toByteView(blob)));

        // Append more values to the same run in a second call.
        std::uint32_t moreElems[2] = {60, 70};
        ByteBuffer moreBlob(elemSize * 2);
        std::memcpy(moreBlob.data(), moreElems, moreBlob.size());
        CHECK_OK(cur.putMultiple(toByteView("fixed"), elemSize, toByteView(moreBlob)));

        CHECK_OK(wtxn.commit());
    }
    {
        auto rtxnR = env.beginRead();
        CHECK(rtxnR.ok());
        txn::ReadTransaction rtxn = rtxnR.moveValue();
        btree::Cursor cur = rtxn.openCursor();
        cur.setFlags(DbFlags::DupFixed);
        auto seek = cur.seekExact(toByteView("fixed"));
        CHECK(seek.ok() && seek.value());
        auto first = cur.firstDup();
        CHECK(first.ok() && first.value());

        std::uint32_t expected[7] = {10, 20, 30, 40, 50, 60, 70};
        for (int i = 0; i < 7; ++i) {
            auto v = cur.valueView();
            CHECK(v.ok());
            if (v.ok()) {
                CHECK(v.value().size() == 4);
                std::uint32_t got = 0;
                std::memcpy(&got, v.value().data(), 4);
                CHECK(got == expected[i]);
            }
            if (i < 6) {
                auto n = cur.nextDup();
                CHECK(n.ok() && n.value());
            }
        }
        // No 8th element.
        auto past = cur.nextDup();
        CHECK(past.ok() && !past.value());
    }

    CHECK_OK(env.close());
    TEST_MAIN_END();
}
