// Incremental backup + encryption together (the gap flagged after the
// initial crypto-suite pass): incrementalDumpToFile always captures
// decrypted plaintext (via the transparent PageManager::getPage()), so the
// fix lives entirely in incrementalLoadFromFile -- it must call
// PageManager::finalizePage() on each loaded page so an encrypted
// destination actually (re-)encrypts that plaintext into its own mmap +
// sidecar, instead of leaving it stranded in PageManager's in-process
// plaintext cache. See IncrementalLoadOptions::envOptions, which lets the
// destination be (re)opened with the right encryption key.
#include "test_common.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/backup/maintenance.hpp"
#include "annpocketdb/encryption/page_cipher.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;
using namespace annpocketdb::backup;

namespace {

std::string key(int i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "k%04d", i);
    return std::string(buf);
}
std::string val(int i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "v%04d", i);
    return std::string(buf);
}

// Bounded prefix only -- see test_encrypted_db.cpp's copy of this helper for
// why: .apdb files are sparse and pre-extended to maxDatabaseSize (1 TiB by
// default), so reading "the whole file" would try to slurp that reservation.
bool fileContainsBytes(const std::filesystem::path& path, const std::string& needle) {
    constexpr std::size_t kMaxScan = 16 * 1024 * 1024;
    std::ifstream in(path, std::ios::binary);
    std::vector<char> buf(kMaxScan);
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    std::size_t got = static_cast<std::size_t>(in.gcount());
    if (got == 0 || needle.empty()) return false;
    return std::search(buf.begin(), buf.begin() + static_cast<long>(got), needle.begin(), needle.end()) !=
           buf.begin() + static_cast<long>(got);
}

}  // namespace

int main() {
    auto srcPath = anntest::scratchDbPath("incbackup_enc_src.apdb");
    auto dstPath = anntest::scratchDbPath("incbackup_enc_dst.apdb");
    auto dump1 = anntest::scratchDbPath("incbackup_enc_1.apinc");
    auto dump2 = anntest::scratchDbPath("incbackup_enc_2.apinc");

    encryption::Key theKey{};
    theKey.fill(0x55);

    EnvironmentOptions encOpts;
    encOpts.enableEncryption = true;
    encOpts.encryptionKey = theKey;

    auto srcR = Environment::open(srcPath, encOpts);
    CHECK(srcR.ok());
    if (!srcR) TEST_MAIN_END();
    Environment src = srcR.moveValue();

    // Batch 1: 30 keys, one of them a distinctive marker we can grep for.
    const std::string marker = "ENC_BACKUP_MARKER_needle_0723_unique_string";
    {
        auto wtxnR = src.beginWrite();
        CHECK(wtxnR.ok());
        if (!wtxnR) TEST_MAIN_END();
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        for (int i = 0; i < 30; ++i) {
            CHECK_OK(wtxn.put(toByteView(key(i)), toByteView(val(i)), true));
        }
        CHECK_OK(wtxn.put(toByteView("marker"), toByteView(marker), true));
        CHECK_OK(wtxn.commit());
    }
    TxnId afterBatch1 = src.pageManager().lastCommittedTxnId();

    // Fresh encrypted destination (same key): create, note its starting
    // txnId, close so the loader can reopen it exclusively.
    TxnId freshBaseline = 0;
    {
        auto dstR = Environment::open(dstPath, encOpts);
        CHECK(dstR.ok());
        if (dstR.ok()) {
            freshBaseline = dstR.value().pageManager().lastCommittedTxnId();
            CHECK_OK(dstR.value().close());
        }
    }
    CHECK(freshBaseline != 0);

    // Full copy relative to the fresh destination's own starting point.
    auto dump1R = src.incrementalDumpToFile(freshBaseline, dump1);
    CHECK(dump1R.ok());
    if (!dump1R) TEST_MAIN_END();
    IncrementalDumpReport rep1 = dump1R.value();
    CHECK(rep1.snapshotTxnId == afterBatch1);

    IncrementalLoadOptions loadOpts;
    loadOpts.envOptions = &encOpts;  // destination must be reopened with the same key
    auto load1R = incrementalLoadFromFile(dump1, dstPath, loadOpts);
    CHECK(load1R.ok());
    if (!load1R) TEST_MAIN_END();
    CHECK(load1R.value().pagesLoaded == rep1.pagesDumped);

    // The raw destination file must be genuinely re-encrypted -- the
    // plaintext marker must not appear in it (proving finalizePage's
    // encryptAndPersist step actually ran, not just a plaintext copy).
    CHECK(!fileContainsBytes(dstPath, marker));

    // Verify batch 1 landed correctly, readable with the correct key.
    {
        auto dstR = Environment::open(dstPath, encOpts);
        CHECK(dstR.ok());
        if (dstR.ok()) {
            Environment dst = dstR.moveValue();
            CHECK(dst.pageManager().lastCommittedTxnId() == afterBatch1);
            for (int i = 0; i < 30; ++i) {
                auto v = dst.get(toByteView(key(i)));
                CHECK(v.ok());
                if (v.ok()) CHECK(toStringView(toByteView(v.value())) == val(i));
            }
            auto m = dst.get(toByteView("marker"));
            CHECK(m.ok());
            if (m.ok()) CHECK(toStringView(toByteView(m.value())) == marker);
            CHECK_OK(dst.close());
        }
    }

    // Wrong key must fail to read (not silently return garbage).
    {
        EnvironmentOptions wrongOpts;
        wrongOpts.enableEncryption = true;
        wrongOpts.encryptionKey.fill(0xAA);
        auto dstR = Environment::open(dstPath, wrongOpts);
        if (dstR.ok()) {
            Environment dst = dstR.moveValue();
            auto v = dst.get(toByteView(key(0)));
            CHECK(!v.ok());
            CHECK_OK(dst.close());
        }
    }

    // Batch 2 in the source: overwrite a couple of batch 1's keys, add 15 new ones.
    {
        auto wtxnR = src.beginWrite();
        CHECK(wtxnR.ok());
        if (!wtxnR) TEST_MAIN_END();
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        for (int i = 0; i < 3; ++i) {
            CHECK_OK(wtxn.put(toByteView(key(i)), toByteView(val(i) + "_updated"), true));
        }
        for (int i = 30; i < 45; ++i) {
            CHECK_OK(wtxn.put(toByteView(key(i)), toByteView(val(i)), true));
        }
        CHECK_OK(wtxn.commit());
    }
    TxnId afterBatch2 = src.pageManager().lastCommittedTxnId();
    CHECK(afterBatch2 > afterBatch1);

    // Delta-only increment relative to afterBatch1, applied on top of the
    // already-encrypted, already-loaded destination.
    auto dump2R = src.incrementalDumpToFile(afterBatch1, dump2);
    CHECK(dump2R.ok());
    if (!dump2R) TEST_MAIN_END();

    auto load2R = incrementalLoadFromFile(dump2, dstPath, loadOpts);
    CHECK(load2R.ok());
    if (!load2R) TEST_MAIN_END();

    // Final destination must match the source exactly, still only readable
    // with the correct key.
    {
        auto dstR = Environment::open(dstPath, encOpts);
        CHECK(dstR.ok());
        if (dstR.ok()) {
            Environment dst = dstR.moveValue();
            CHECK(dst.pageManager().lastCommittedTxnId() == afterBatch2);
            for (int i = 0; i < 3; ++i) {
                auto v = dst.get(toByteView(key(i)));
                CHECK(v.ok());
                if (v.ok()) CHECK(toStringView(toByteView(v.value())) == val(i) + "_updated");
            }
            for (int i = 3; i < 45; ++i) {
                auto v = dst.get(toByteView(key(i)));
                CHECK(v.ok());
                if (v.ok()) CHECK(toStringView(toByteView(v.value())) == val(i));
            }
            CHECK_OK(dst.close());
        }
    }

    CHECK_OK(src.close());
    TEST_MAIN_END();
}
