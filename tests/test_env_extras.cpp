// LMDB-parity extras on Environment (final inventory pass):
// openOrCreateSubDatabase (idempotent get-or-create, flags ignored on an
// existing database), sync(), maxKeySize(), and compactCopy().
#include "test_common.hpp"

#include <string>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/backup/maintenance.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;
using namespace annpocketdb::btree;

int main() {
    auto path = anntest::scratchDbPath("env_extras.apdb");
    auto envR = Environment::open(path);
    CHECK(envR.ok());
    if (!envR) TEST_MAIN_END();
    Environment env = envR.moveValue();

    // ---- openOrCreateSubDatabase: creates on first call ----
    auto first = env.openOrCreateSubDatabase("widgets", DbFlags::DupSort);
    CHECK(first.ok());
    if (first.ok()) {
        CHECK(first.value().flags() == DbFlags::DupSort);
        CHECK_OK(first.value().put(toByteView("k"), toByteView("v")));
    }

    // Second call with DIFFERENT flags: must open the EXISTING database and
    // ignore the new flags (matches LMDB's open_db() immutability), not
    // error and not silently reconfigure it.
    auto second = env.openOrCreateSubDatabase("widgets", DbFlags::None);
    CHECK(second.ok());
    if (second.ok()) {
        CHECK(second.value().flags() == DbFlags::DupSort);  // unchanged
        auto v = second.value().get(toByteView("k"));
        CHECK(v.ok());
        if (v.ok()) CHECK(toStringView(toByteView(v.value())) == "v");
    }

    // ---- sync(): should not error, regardless of force ----
    CHECK_OK(env.sync(false));
    CHECK_OK(env.sync(true));

    // ---- maxKeySize(): a positive, sane bound derived from pageSize ----
    std::size_t maxKey = env.maxKeySize();
    CHECK(maxKey > 0 && maxKey < 4096);

    // Put a few more keys so the compact copy has real content to carry over.
    for (int i = 0; i < 20; ++i) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "key%04d", i);
        char vbuf[16];
        std::snprintf(vbuf, sizeof(vbuf), "val%04d", i);
        CHECK_OK(env.put(toByteView(std::string(buf)), toByteView(std::string(vbuf))));
    }

    // ---- compactCopy(): fresh copy carries over every live record ----
    auto compactPath = anntest::scratchDbPath("env_extras_compact.apdb");
    auto compactR = env.compactCopyTo(compactPath);
    CHECK(compactR.ok());
    if (compactR.ok()) CHECK(compactR.value().entries >= 20);

    CHECK_OK(env.close());

    {
        auto copyR = Environment::open(compactPath);
        CHECK(copyR.ok());
        if (copyR.ok()) {
            Environment copy = copyR.moveValue();
            for (int i = 0; i < 20; ++i) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "key%04d", i);
                char vbuf[16];
                std::snprintf(vbuf, sizeof(vbuf), "val%04d", i);
                auto v = copy.get(toByteView(std::string(buf)));
                CHECK(v.ok());
                if (v.ok()) CHECK(toStringView(toByteView(v.value())) == std::string(vbuf));
            }
            CHECK_OK(copy.close());
        }
    }

    // Re-running compactCopy at the same destination without forceOverwrite
    // must fail rather than silently clobber it.
    {
        auto envR2 = Environment::open(path);
        CHECK(envR2.ok());
        if (envR2.ok()) {
            Environment env2 = envR2.moveValue();
            auto again = env2.compactCopyTo(compactPath, /*forceOverwrite=*/false);
            CHECK(!again.ok());
            CHECK_OK(env2.close());
        }
    }

    TEST_MAIN_END();
}
