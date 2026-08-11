// Basic put/get/remove smoke test.
#include "test_common.hpp"

#include "annpocketdb/api/environment.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;

int main() {
    auto path = anntest::scratchDbPath("basic_kv.apdb");

    auto envR = Environment::open(path);
    CHECK(envR.ok());
    if (!envR) TEST_MAIN_END();
    Environment env = envR.moveValue();

    CHECK_OK(env.put(toByteView("hello"), toByteView("world")));
    auto g = env.get(toByteView("hello"));
    CHECK(g.ok());
    if (g.ok()) CHECK(toStringView(toByteView(g.value())) == "world");

    // Overwrite.
    CHECK_OK(env.put(toByteView("hello"), toByteView("world2")));
    auto g2 = env.get(toByteView("hello"));
    CHECK(g2.ok());
    if (g2.ok()) CHECK(toStringView(toByteView(g2.value())) == "world2");

    // Missing key.
    auto missing = env.get(toByteView("nope"));
    CHECK(!missing.ok());
    CHECK(missing.status().code() == StatusCode::NotFound);

    // Remove.
    CHECK_OK(env.remove(toByteView("hello")));
    auto g3 = env.get(toByteView("hello"));
    CHECK(!g3.ok());

    // Many keys.
    for (int i = 0; i < 500; ++i) {
        std::string k = "key" + std::to_string(i);
        std::string v = "value" + std::to_string(i);
        CHECK_OK(env.put(toByteView(k), toByteView(v)));
    }
    for (int i = 0; i < 500; ++i) {
        std::string k = "key" + std::to_string(i);
        std::string v = "value" + std::to_string(i);
        auto r = env.get(toByteView(k));
        CHECK(r.ok());
        if (r.ok()) CHECK(toStringView(toByteView(r.value())) == v);
    }

    CHECK_OK(env.close());
    TEST_MAIN_END();
}
