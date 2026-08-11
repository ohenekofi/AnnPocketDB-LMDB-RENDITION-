// Cursor Append fast-path smoke test: bulk pre-sorted insert works and
// reads back correctly; an out-of-order Append call is rejected rather than
// silently falling back or corrupting the tree.
#include "test_common.hpp"

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/btree/cursor.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;
using namespace annpocketdb::btree;

static std::string zeroPad(int i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "k%06d", i);
    return std::string(buf);
}

int main() {
    auto path = anntest::scratchDbPath("append.apdb");

    auto envR = Environment::open(path);
    CHECK(envR.ok());
    if (!envR) TEST_MAIN_END();
    Environment env = envR.moveValue();

    constexpr int kCount = 2000;
    {
        auto wtxnR = env.beginWrite();
        CHECK(wtxnR.ok());
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        btree::Cursor cur = wtxn.openCursor();
        for (int i = 0; i < kCount; ++i) {
            std::string k = zeroPad(i);
            std::string v = std::to_string(i);
            Status s = cur.put(toByteView(k), toByteView(v), CursorPutFlags::Append);
            CHECK_OK(s);
        }
        CHECK_OK(wtxn.commit());
    }

    for (int i = 0; i < kCount; i += 137) {  // spot-check, not exhaustive
        auto v = env.get(toByteView(zeroPad(i)));
        CHECK(v.ok());
        if (v.ok()) CHECK(toStringView(toByteView(v.value())) == std::to_string(i));
    }

    // Out-of-order append must fail cleanly, not corrupt anything.
    {
        auto wtxnR = env.beginWrite();
        CHECK(wtxnR.ok());
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        btree::Cursor cur = wtxn.openCursor();
        Status s = cur.put(toByteView(zeroPad(5)), toByteView("bad"), CursorPutFlags::Append);
        CHECK(!s.ok());
        CHECK(s.code() == StatusCode::InvalidArgument);
        wtxn.abort();
    }

    // Data must be unaffected by the rejected append above.
    auto stillThere = env.get(toByteView(zeroPad(1999)));
    CHECK(stillThere.ok());
    if (stillThere.ok()) CHECK(toStringView(toByteView(stillThere.value())) == "1999");

    CHECK_OK(env.close());
    TEST_MAIN_END();
}
