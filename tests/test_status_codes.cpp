// LMDB-parity granular error codes (final inventory pass): verifies the new
// StatusCode values are actually reachable from real operations, not just
// declared -- BadValSize, DbsFull, BadDbi, InvalidFile.
#include "test_common.hpp"

#include <fstream>
#include <string>

#include "annpocketdb/api/environment.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;
using namespace annpocketdb::btree;

int main() {
    // ---- BadValSize: a key larger than maxKeySize() is rejected specifically ----
    {
        auto path = anntest::scratchDbPath("status_badvalsize.apdb");
        auto envR = Environment::open(path);
        CHECK(envR.ok());
        if (envR.ok()) {
            Environment env = envR.moveValue();
            std::string hugeKey(env.maxKeySize() + 1, 'k');
            Status s = env.put(toByteView(hugeKey), toByteView("v"));
            CHECK(!s.ok());
            CHECK(s.code() == StatusCode::BadValSize);
            CHECK_OK(env.close());
        }
    }

    // ---- DbsFull: maxNamedDatabases=1, a second create fails specifically ----
    {
        auto path = anntest::scratchDbPath("status_dbsfull.apdb");
        EnvironmentOptions opts;
        opts.maxNamedDatabases = 1;
        auto envR = Environment::open(path, opts);
        CHECK(envR.ok());
        if (envR.ok()) {
            Environment env = envR.moveValue();
            auto first = env.createSubDatabase("first");
            CHECK(first.ok());
            auto second = env.createSubDatabase("second");
            CHECK(!second.ok());
            CHECK(second.status().code() == StatusCode::DbsFull);
            CHECK_OK(env.close());
        }
    }

    // ---- BadDbi: a handle invalidated by close() surfaces BadDbi, not just NotFound ----
    {
        auto path = anntest::scratchDbPath("status_baddbi.apdb");
        auto envR = Environment::open(path);
        CHECK(envR.ok());
        if (envR.ok()) {
            Environment env = envR.moveValue();
            auto subR = env.createSubDatabase("widgets");
            CHECK(subR.ok());
            if (subR.ok()) {
                SubDatabase sub = subR.moveValue();
                CHECK_OK(sub.put(toByteView("k"), toByteView("v")));
                sub.close();
                auto after = sub.get(toByteView("k"));
                CHECK(!after.ok());
                CHECK(after.status().code() == StatusCode::BadDbi);
            }
            CHECK_OK(env.close());
        }
    }

    // ---- InvalidFile: opening a bogus (non-AnnPocketDb) file surfaces InvalidFile ----
    {
        auto path = anntest::scratchDbPath("status_invalidfile.apdb");
        {
            std::ofstream junk(path, std::ios::binary | std::ios::trunc);
            junk << std::string(8192, '\xAB');
        }
        auto envR = Environment::open(path);
        CHECK(!envR.ok());
        CHECK(envR.status().code() == StatusCode::InvalidFile);
    }

    TEST_MAIN_END();
}
