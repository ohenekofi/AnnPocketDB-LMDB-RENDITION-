// Two-phase commit smoke test: prepare() + commit() finalizes and persists;
// prepare() + abort() discards, even though prepare() already wrote a
// durable (but unpublished) meta slot.
#include "test_common.hpp"

#include "annpocketdb/api/environment.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;

int main() {
    auto path = anntest::scratchDbPath("twopc.apdb");

    // ---- prepare() then commit() ----
    {
        auto envR = Environment::open(path);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        auto wtxnR = env.beginWrite();
        CHECK(wtxnR.ok());
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        CHECK_OK(wtxn.put(toByteView("committed"), toByteView("yes")));
        CHECK_OK(wtxn.prepare());
        CHECK(wtxn.prepared());
        CHECK_OK(wtxn.commit());

        auto v = env.get(toByteView("committed"));
        CHECK(v.ok());
        CHECK_OK(env.close());
    }
    // Reopen fresh to confirm the finalize was actually durable, not just
    // reflected in the in-memory snapshot of the same Environment object.
    {
        auto envR = Environment::open(path);
        CHECK(envR.ok());
        if (envR.ok()) {
            auto v = envR.value().get(toByteView("committed"));
            CHECK(v.ok());
            CHECK_OK(envR.value().close());
        }
    }

    // ---- prepare() then abort() ----
    {
        auto envR = Environment::open(path);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();

        auto wtxnR = env.beginWrite();
        CHECK(wtxnR.ok());
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        CHECK_OK(wtxn.put(toByteView("aborted"), toByteView("no")));
        CHECK_OK(wtxn.prepare());
        CHECK_OK(wtxn.abort());

        auto v = env.get(toByteView("aborted"));
        CHECK(!v.ok());
        CHECK(v.status().code() == StatusCode::NotFound);

        // A fresh write transaction must still work normally afterward (the
        // writer gate was correctly released by abort()).
        auto wtxn2R = env.beginWrite();
        CHECK(wtxn2R.ok());
        if (wtxn2R.ok()) {
            CHECK_OK(wtxn2R.value().put(toByteView("after"), toByteView("ok")));
            CHECK_OK(wtxn2R.value().commit());
        }
        auto after = env.get(toByteView("after"));
        CHECK(after.ok());

        CHECK_OK(env.close());
    }

    TEST_MAIN_END();
}
