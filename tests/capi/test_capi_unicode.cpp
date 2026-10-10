// C API with a non-ASCII (Unicode) data path. apdb_env_open's `path` is
// UTF-8 on every platform (see annpocketdb.h); before that was made explicit,
// Windows read it in the ANSI code page, so the store and its .wal/.lock/.enc
// sidecars landed under a mangled name (or open failed outright).
//
// C++ only for std::filesystem (to check the sidecars on disk under the exact
// name); the library is still reached exclusively through the C header.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>

#include "annpocketdb_c/annpocketdb.h"

namespace fs = std::filesystem;

static int g_failures = 0;

#define ASSERT(cond)                                                        \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

namespace {

// Directory "Ọbẹ_データ_Ünïcødé" / file "dàtà_ẹ_データ.apdb", as UCNs so the
// source encoding doesn't matter.
constexpr const char8_t* kDirName =
    u8"Ọbẹ_データ_Ünïcødé";
constexpr const char8_t* kDataName = u8"dàtà_ẹ_データ.apdb";

fs::path withSuffix(fs::path p, const char* suffix) {
    p += suffix;
    return p;
}

void openEnv(const std::string& utf8Path, APDB_Env* env) {
    static std::uint8_t key[32];
    for (int i = 0; i < 32; ++i) key[i] = static_cast<std::uint8_t>(0x30 + i);

    APDB_EnvOptions opts{};
    opts.flags = APDB_ENV_WAL;
#if defined(APDB_TEST_WITH_ENCRYPTION)
    opts.flags |= APDB_ENV_ENCRYPTION;
    opts.encryption_key = key;
#endif
    APDB_rc rc = apdb_env_open(utf8Path.c_str(), &opts, env);
    if (rc != APDB_OK) std::fprintf(stderr, "apdb_env_open: %s\n", apdb_errmsg(rc));
    ASSERT(rc == APDB_OK);
}

void run() {
    const fs::path root = fs::temp_directory_path() / "annpocketdb_tests" / "capi_unicode_root";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path dir = root / fs::path(kDirName);
    fs::create_directories(dir);
    const fs::path dataPath = dir / fs::path(kDataName);

    // The C API's contract: a UTF-8 byte string, whatever the platform.
    const std::u8string u8 = dataPath.u8string();
    const std::string utf8Path(reinterpret_cast<const char*>(u8.data()), u8.size());

    constexpr int kKeys = 20;
    {
        APDB_Env env = nullptr;
        openEnv(utf8Path, &env);
        if (!env) return;
        for (int i = 0; i < kKeys; ++i) {
            std::string k = "key-" + std::to_string(i);
            std::string v = "value-" + std::to_string(i);
            ASSERT(apdb_put(env, k.data(), k.size(), v.data(), v.size()) == APDB_OK);
        }
        ASSERT(apdb_env_close(env) == APDB_OK);
    }
    {
        APDB_Env env = nullptr;
        openEnv(utf8Path, &env);
        if (!env) return;
        for (int i = 0; i < kKeys; ++i) {
            std::string k = "key-" + std::to_string(i);
            std::string v = "value-" + std::to_string(i);
            const void* data = nullptr;
            size_t size = 0;
            ASSERT(apdb_get(env, k.data(), k.size(), &data, &size) == APDB_OK);
            ASSERT(size == v.size() && data && std::memcmp(data, v.data(), size) == 0);
        }
        ASSERT(apdb_env_close(env) == APDB_OK);
    }

    // Exact Unicode names, next to the data file, nothing else anywhere.
    std::set<fs::path> expected = {
        fs::path(kDataName),
        withSuffix(fs::path(kDataName), ".wal"),
        withSuffix(fs::path(kDataName), ".lock"),
    };
#if defined(APDB_TEST_WITH_ENCRYPTION)
    expected.insert(withSuffix(fs::path(kDataName), ".enc"));
#endif
    std::set<fs::path> actual;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        actual.insert(it->path().filename());
    }
    ASSERT(actual == expected);

    std::set<fs::path> rootEntries;
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        rootEntries.insert(it->path().filename());
    }
    ASSERT(rootEntries == std::set<fs::path>{fs::path(kDirName)});
}

}  // namespace

int main() {
    try {
        run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[FAIL] exception escaped: %s\n", e.what());
        ++g_failures;
    }
    if (g_failures == 0) std::printf("[PASS] %s\n", __FILE__);
    else std::printf("[FAIL] %s (%d failure(s))\n", __FILE__, g_failures);
    return g_failures;
}
