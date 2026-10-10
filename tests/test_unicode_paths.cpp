// Non-ASCII (Unicode) data paths, end to end. On Windows,
// std::filesystem::path::string() narrows through the ANSI code page, so any
// sidecar path rebuilt from it (".wal", ".lock", ".sha256", ".enc") either
// named a different file or threw std::system_error out of a Status API.
// Every name below uses characters outside common ANSI code pages
// (Yoruba dot-below vowels, Japanese kana) so this fails on a CP1252 system
// if any path goes through std::string again.
//
//   1. WAL + multi-process + (if built) SHA-256 + encryption: write keys and
//      a blob, close, reopen, read back; every sidecar exists under the exact
//      Unicode name next to the data file and nothing appears elsewhere.
//   2. dump / load / compactCopy / repair / verify with Unicode source and
//      destination paths, including the forceOverwrite ".lock" removal.
//   3. Missing Unicode path opened read-only: Status error (no exception),
//      message carries the path as valid UTF-8. Same for missing dumps.
//   4. A stray non-ASCII file in the WAL directory is ignored on reopen.
//
// All string literals are u8"" with universal-character-names, so the test
// doesn't depend on the compiler's source/execution character set.
#include "test_common.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <fstream>
#include <set>
#include <string>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/backup/maintenance.hpp"
#include "annpocketdb/util/path.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;
using namespace annpocketdb::backup;
using annpocketdb::util::pathToUtf8;
using annpocketdb::util::withSuffix;

namespace fs = std::filesystem;

namespace {

// "Ọbẹ_データ_Ünïcødé"
constexpr const char8_t* kDirName =
    u8"Ọbẹ_データ_Ünïcødé";
// "dàtà_ẹ_データ.apdb"
constexpr const char8_t* kDataName = u8"dàtà_ẹ_データ.apdb";

// Structural UTF-8 check: lead byte, then exactly `n` 10xxxxxx continuation
// bytes, all present.
bool isValidUtf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t n = 0;  // continuation bytes after the lead
        if (c < 0x80) n = 0;
        else if ((c & 0xE0) == 0xC0) n = 1;
        else if ((c & 0xF0) == 0xE0) n = 2;
        else if ((c & 0xF8) == 0xF0) n = 3;
        else return false;
        if (n > s.size() - i - 1) return false;  // i < size, so no underflow
        for (std::size_t k = 1; k <= n; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += n + 1;
    }
    return true;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::set<fs::path> listNames(const fs::path& dir) {
    std::set<fs::path> names;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        names.insert(it->path().filename());
    }
    return names;
}

#if defined(ANNPOCKETDB_WITH_ENCRYPTION)
std::array<std::uint8_t, 32> testKey() {
    std::array<std::uint8_t, 32> k{};
    for (std::size_t i = 0; i < k.size(); ++i) k[i] = static_cast<std::uint8_t>(0xA0 + i);
    return k;
}
#endif

EnvironmentOptions fullOptions() {
    EnvironmentOptions opts;
    opts.enableWal = true;
    opts.multiProcess = true;
#if defined(ANNPOCKETDB_WITH_ENCRYPTION)
    opts.enableSha256Checksum = true;
    opts.enableEncryption = true;
    opts.encryptionKey = testKey();
#endif
    return opts;
}

ByteBuffer blobData() {
    ByteBuffer b(20000);
    for (std::size_t i = 0; i < b.size(); ++i) b[i] = static_cast<std::byte>((i * 7) % 253);
    return b;
}

void writeGarbage(const fs::path& p) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << "not an annpocketdb file -- must be removed by forceOverwrite";
}

void checkKeys(Environment& env, int count) {
    for (int i = 0; i < count; ++i) {
        std::string k = "key-" + std::to_string(i);
        auto r = env.get(toByteView(k));
        CHECK(r.ok());
        if (r.ok()) {
            std::string v = "value-" + std::to_string(i);
            CHECK(r.value() == toByteBuffer(toByteView(v)));
        }
    }
}

void run() {
    // ASCII root that only this test writes to, so "no stray files elsewhere"
    // can be checked exactly: after everything, it holds just the Unicode dir.
    const fs::path root = fs::temp_directory_path() / "annpocketdb_tests" / "unicode_paths_root";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / fs::path(kDirName));
    const fs::path dir = root / fs::path(kDirName);
    const fs::path dataPath = dir / fs::path(kDataName);

    constexpr int kKeys = 50;
    const ByteBuffer blob = blobData();

    // ---- 1. full-feature round trip -------------------------------------
    {
        auto envR = Environment::open(dataPath, fullOptions());
        CHECK_OK(envR.status());
        if (!envR) return;
        Environment env = envR.moveValue();
        for (int i = 0; i < kKeys; ++i) {
            std::string k = "key-" + std::to_string(i);
            std::string v = "value-" + std::to_string(i);
            CHECK_OK(env.put(toByteView(k), toByteView(v)));
        }
        CHECK(env.putBlob(toByteView(std::string("blob")), toByteView(blob)).ok());
        CHECK_OK(env.close());
    }
    {
        auto envR = Environment::open(dataPath, fullOptions());
        CHECK_OK(envR.status());
        if (!envR) return;
        Environment env = envR.moveValue();
        checkKeys(env, kKeys);
        auto b = env.getBlob(toByteView(std::string("blob")));
        CHECK(b.ok());
        if (b.ok()) CHECK(b.value() == blob);
        CHECK_OK(env.close());
    }

    {
        std::set<fs::path> expected = {
            fs::path(kDataName),
            withSuffix(fs::path(kDataName), ".wal"),
            withSuffix(fs::path(kDataName), ".lock"),
        };
#if defined(ANNPOCKETDB_WITH_ENCRYPTION)
        expected.insert(withSuffix(fs::path(kDataName), ".sha256"));
        expected.insert(withSuffix(fs::path(kDataName), ".enc"));
#endif
        for (const auto& name : expected) {
            if (!fs::exists(dir / name)) {
                anntest::reportFailure(__FILE__, __LINE__, "missing sidecar: " + pathToUtf8(dir / name));
            }
        }
        CHECK(fs::is_directory(withSuffix(dataPath, ".wal")));
        CHECK(listNames(dir) == expected);
        CHECK(listNames(root) == std::set<fs::path>{fs::path(kDirName)});
    }

    // ---- 4. stray non-ASCII file in the WAL directory -------------------
    {
        // "ストレイ_ẹ.seg" -- ends in .seg on purpose, so it reaches the parser.
        writeGarbage(withSuffix(dataPath, ".wal") / fs::path(u8"ストレイ_ẹ.seg"));
        auto envR = Environment::open(dataPath, fullOptions());
        CHECK_OK(envR.status());
        if (envR) {
            Environment env = envR.moveValue();
            checkKeys(env, kKeys);
            CHECK_OK(env.close());
        }
    }

    // ---- 2. backup / dump / restore with Unicode paths ------------------
    // A plain (default-options) store, because dumpFile/verifyFile/repair
    // open their source with default options (no encryption key).
    const fs::path plainPath = dir / fs::path(u8"pláin_プレーン.apdb");
    {
        auto envR = Environment::open(plainPath);
        CHECK_OK(envR.status());
        if (!envR) return;
        Environment env = envR.moveValue();
        for (int i = 0; i < kKeys; ++i) {
            std::string k = "key-" + std::to_string(i);
            std::string v = "value-" + std::to_string(i);
            CHECK_OK(env.put(toByteView(k), toByteView(v)));
        }

        // compactCopy: tmp dump is destPath + ".compact_tmp"
        const fs::path compactPath = dir / fs::path(u8"cömpact_コピー.apdb");
        auto cR = env.compactCopyTo(compactPath);
        CHECK_OK(cR.status());
        CHECK(!fs::exists(withSuffix(compactPath, ".compact_tmp")));
        CHECK_OK(env.close());

        auto vR = verifyFile(compactPath);
        CHECK_OK(vR.status());
        if (vR) CHECK(vR.value().ok());
    }

    const fs::path dumpPath = dir / fs::path(u8"dümp_ダンプ.apdump");
    {
        auto dR = dumpFile(plainPath, dumpPath);
        CHECK_OK(dR.status());
        if (dR) CHECK(dR.value().entries == static_cast<std::uint64_t>(kKeys));
        CHECK(fs::exists(dumpPath));
    }
    {
        // Pre-existing garbage destination AND garbage .lock: forceOverwrite
        // must remove both (backup.cpp loadFromFile). A surviving garbage
        // .lock makes the subsequent open fail with "unsupported lock file
        // format", so a pass here proves the Unicode .lock path was right.
        const fs::path restored = dir / fs::path(u8"réstored_復元.apdb");
        writeGarbage(restored);
        writeGarbage(withSuffix(restored, ".lock"));
        LoadOptions lo;
        lo.forceOverwrite = true;
        auto lR = loadFromFile(dumpPath, restored, lo);
        CHECK_OK(lR.status());
        if (lR) CHECK(lR.value().entries == static_cast<std::uint64_t>(kKeys));

        auto envR = Environment::open(restored);
        CHECK_OK(envR.status());
        if (envR) {
            Environment env = envR.moveValue();
            checkKeys(env, kKeys);
            CHECK_OK(env.close());
        }
    }
    {
        // Same for repair (backup.cpp repair, dest ".lock" removal).
        const fs::path repaired = dir / fs::path(u8"rẹpaïred_修復.apdb");
        writeGarbage(repaired);
        writeGarbage(withSuffix(repaired, ".lock"));
        RepairOptions ro;
        ro.forceOverwrite = true;
        auto rR = repair(plainPath, repaired, ro);
        CHECK_OK(rR.status());

        auto envR = Environment::open(repaired);
        CHECK_OK(envR.status());
        if (envR) {
            Environment env = envR.moveValue();
            checkKeys(env, kKeys);
            CHECK_OK(env.close());
        }
    }

    // ---- 3. error messages carry the Unicode path as UTF-8 --------------
    {
        const fs::path missing = dir / fs::path(u8"mïssing_ナシ.apdb");
        EnvironmentOptions ro;
        ro.readOnly = true;
        auto envR = Environment::open(missing, ro);
        CHECK(!envR.ok());
        if (!envR) {
            const std::string& msg = envR.status().message();
            CHECK(isValidUtf8(msg));
            CHECK(contains(msg, pathToUtf8(missing)));
        }
        CHECK(!fs::exists(missing));
    }
    {
        const fs::path missingDump = dir / fs::path(u8"nö_ダンプ.apdump");
        auto lR = loadFromFile(missingDump, dir / fs::path(u8"nêver.apdb"), LoadOptions{});
        CHECK(!lR.ok());
        if (!lR) {
            CHECK(isValidUtf8(lR.status().message()));
            CHECK(contains(lR.status().message(), pathToUtf8(missingDump)));
        }

        auto iR = incrementalLoadFromFile(missingDump, plainPath, IncrementalLoadOptions{});
        CHECK(!iR.ok());
        if (!iR) {
            CHECK(isValidUtf8(iR.status().message()));
            CHECK(contains(iR.status().message(), pathToUtf8(missingDump)));
        }
    }

    // Nothing leaked outside the Unicode directory (e.g. a sidecar built
    // from a mangled, code-page-narrowed name).
    CHECK(listNames(root) == std::set<fs::path>{fs::path(kDirName)});
}

}  // namespace

int main() {
    // A path conversion that throws is exactly the bug class under test:
    // report it as a failure instead of letting it abort the process.
    try {
        run();
    } catch (const std::exception& e) {
        anntest::reportFailure(__FILE__, __LINE__, std::string("exception escaped: ") + e.what());
    } catch (...) {
        anntest::reportFailure(__FILE__, __LINE__, "unknown exception escaped");
    }
    TEST_MAIN_END();
}
