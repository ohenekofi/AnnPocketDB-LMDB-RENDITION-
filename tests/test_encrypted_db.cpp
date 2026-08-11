// Crypto suite (Item 7), full Environment-level wiring: page-level
// XChaCha20-Poly1305 encryption and the optional SHA-256 checksum bolt-on,
// exercised through the real public API (not just the primitives -- see
// test_crypto_primitives.cpp for those). Verifies:
//   - round-trip put/get through an encrypted Environment, across a close+reopen
//   - the raw on-disk file does NOT contain the plaintext marker (real
//     encryption, not just an opaque wrapper), while an equivalent
//     unencrypted DB's raw file DOES (sanity-checks the test method itself)
//   - a bit-flipped ciphertext page fails decryption (Corruption), not a
//     silent wrong answer
//   - the SHA-256 checksum bolt-on independently catches corruption of its
//     own sidecar file even though CRC32C on the main file is untouched --
//     proving it's additive, not merely piggybacking on CRC
#include "test_common.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/encryption/page_cipher.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;

namespace {

// Reads only a bounded prefix, never the whole file: .apdb files are sparse,
// pre-extended to maxDatabaseSize (1 TiB by default) for pointer-stable
// growth (see platform/mapped_file.hpp) -- naively reading "the whole file"
// via istreambuf_iterator would try to slurp that entire reservation into
// memory. Our tiny test data always lives in the first few pages regardless.
bool fileContainsBytes(const std::filesystem::path& path, const std::string& needle) {
    constexpr std::size_t kMaxScan = 16 * 1024 * 1024;
    std::ifstream in(path, std::ios::binary);
    std::vector<char> buf(kMaxScan);
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    std::size_t got = static_cast<std::size_t>(in.gcount());
    if (got == 0 || needle.empty()) return false;
    auto it = std::search(buf.begin(), buf.begin() + static_cast<long>(got), needle.begin(), needle.end());
    return it != buf.begin() + static_cast<long>(got);
}

// Flips one byte at `offset` in `path`. Returns false if the file is too small.
bool flipByteAt(const std::filesystem::path& path, std::uint64_t offset) {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) return false;
    f.seekg(static_cast<std::streamoff>(offset));
    char c;
    if (!f.get(c)) return false;
    f.seekp(static_cast<std::streamoff>(offset));
    char flipped = static_cast<char>(static_cast<unsigned char>(c) ^ 0xFF);
    f.write(&flipped, 1);
    return true;
}

}  // namespace

namespace {
void trace(const char* what) {
    std::fprintf(stderr, "[trace] %s\n", what);
    std::fflush(stderr);
}
}  // namespace

int main() {
    constexpr std::uint32_t kPageSize = 4096;  // core::kDefaultPageSize
    const std::string marker = "MARKER_VALUE_needle_search_0451_unique_string_for_grep";

    encryption::Key key{};
    key.fill(0x37);

    trace("scratch paths");
    auto encPath = anntest::scratchDbPath("encrypted_db.apdb");
    auto plainPath = anntest::scratchDbPath("plain_db_control.apdb");

    // ---- Control: an ordinary (unencrypted) DB's raw file DOES contain the marker ----
    trace("opening plain control db");
    {
        auto envR = Environment::open(plainPath);
        CHECK(envR.ok());
        if (envR.ok()) {
            Environment env = envR.moveValue();
            trace("plain: put");
            CHECK_OK(env.put(toByteView("k"), toByteView(marker)));
            trace("plain: close");
            CHECK_OK(env.close());
        }
    }
    trace("plain: verifying marker present in raw file");
    CHECK(fileContainsBytes(plainPath, marker));

    // ---- Encrypted DB: put + close ----
    trace("opening encrypted db (fresh)");
    {
        EnvironmentOptions opts;
        opts.enableEncryption = true;
        opts.encryptionKey = key;
        auto envR = Environment::open(encPath, opts);
        CHECK(envR.ok());
        if (!envR) TEST_MAIN_END();
        Environment env = envR.moveValue();
        trace("encrypted: put");
        CHECK_OK(env.put(toByteView("k"), toByteView(marker)));
        trace("encrypted: close");
        CHECK_OK(env.close());
    }

    // The raw encrypted file must NOT contain the plaintext marker.
    trace("encrypted: verifying marker absent from raw file");
    CHECK(!fileContainsBytes(encPath, marker));

    // ---- Reopen with the correct key: round-trips correctly ----
    trace("reopening encrypted db with correct key");
    {
        EnvironmentOptions opts;
        opts.enableEncryption = true;
        opts.encryptionKey = key;
        auto envR = Environment::open(encPath, opts);
        CHECK(envR.ok());
        if (envR.ok()) {
            Environment env = envR.moveValue();
            trace("encrypted: get (correct key)");
            auto v = env.get(toByteView("k"));
            CHECK(v.ok());
            if (v.ok()) CHECK(toStringView(toByteView(v.value())) == marker);
            trace("encrypted: close (correct key reopen)");
            CHECK_OK(env.close());
        }
    }

    // ---- Reopen with the WRONG key: reads fail with an error, not garbage ----
    trace("reopening encrypted db with WRONG key");
    {
        EnvironmentOptions opts;
        opts.enableEncryption = true;
        opts.encryptionKey.fill(0x99);  // wrong key
        auto envR = Environment::open(encPath, opts);
        // open() itself only touches meta pages (never encrypted), so it may
        // succeed; the failure must surface on the first data-page read.
        trace("wrong-key open returned");
        if (envR.ok()) {
            Environment env = envR.moveValue();
            trace("wrong-key: get");
            auto v = env.get(toByteView("k"));
            trace("wrong-key: get returned");
            CHECK(!v.ok());
            CHECK_OK(env.close());
        }
    }

    // ---- Bit-flip a byte in the first data page (page id 2 -- the very
    // first page a fresh DB's first write allocates) and confirm the
    // corruption is detected rather than silently misread ----
    trace("flipping a byte in the encrypted main file");
    {
        std::uint64_t offset = static_cast<std::uint64_t>(2) * kPageSize + 100;
        CHECK(flipByteAt(encPath, offset));

        EnvironmentOptions opts;
        opts.enableEncryption = true;
        opts.encryptionKey = key;
        trace("reopening after bit-flip");
        auto envR = Environment::open(encPath, opts);
        trace("reopen after bit-flip returned");
        if (envR.ok()) {
            Environment env = envR.moveValue();
            trace("corrupted: get");
            auto v = env.get(toByteView("k"));
            trace("corrupted: get returned");
            CHECK(!v.ok());  // AEAD tag must fail, not return corrupted plaintext
            CHECK_OK(env.close());
        }
    }

    // ---- SHA-256 checksum bolt-on, independent of encryption: corrupt only
    // the .sha256 sidecar (main file's CRC32C stays completely valid) and
    // confirm it alone is enough to fail the read ----
    trace("opening checksum-only db (fresh)");
    auto csPath = anntest::scratchDbPath("checksum_db.apdb");
    {
        EnvironmentOptions opts;
        opts.enableSha256Checksum = true;
        auto envR = Environment::open(csPath, opts);
        CHECK(envR.ok());
        if (envR.ok()) {
            Environment env = envR.moveValue();
            trace("checksum: put");
            CHECK_OK(env.put(toByteView("k"), toByteView(marker)));
            trace("checksum: close");
            CHECK_OK(env.close());
        }
    }
    trace("reopening checksum db (normal)");
    {
        // Normal reopen still works (checksum bolt-on doesn't break anything).
        EnvironmentOptions opts;
        opts.enableSha256Checksum = true;
        auto envR = Environment::open(csPath, opts);
        CHECK(envR.ok());
        if (envR.ok()) {
            Environment env = envR.moveValue();
            trace("checksum: get");
            auto v = env.get(toByteView("k"));
            CHECK(v.ok());
            if (v.ok()) CHECK(toStringView(toByteView(v.value())) == marker);
            CHECK_OK(env.close());
        }
    }
    trace("flipping a byte in the .sha256 sidecar");
    {
        auto sidecarPath = std::filesystem::path(csPath.string() + ".sha256");
        CHECK(flipByteAt(sidecarPath, /*offset=*/2 * 32));  // page id 2's 32-byte digest slot

        EnvironmentOptions opts;
        opts.enableSha256Checksum = true;
        trace("reopening checksum db after sidecar corruption");
        auto envR = Environment::open(csPath, opts);
        trace("reopen after sidecar corruption returned");
        if (envR.ok()) {
            Environment env = envR.moveValue();
            trace("checksum-corrupted: get");
            auto v = env.get(toByteView("k"));
            trace("checksum-corrupted: get returned");
            CHECK(!v.ok());  // main file's CRC32C is untouched -- only the checksum bolt-on can catch this
            CHECK_OK(env.close());
        }
    }

    trace("done");
    TEST_MAIN_END();
}
