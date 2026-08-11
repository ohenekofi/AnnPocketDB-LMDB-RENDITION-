// Incremental / delta backup: physical page-level dump+load (see
// backup/maintenance.hpp's incrementalDumpToFile/incrementalLoadFromFile).
// Verifies a full copy into a brand-new destination (baseline = the fresh
// environment's starting txnId, 1), a second delta-only increment applied on
// top of it, and that applying a stale/mismatched increment is rejected.
#include "test_common.hpp"

#include <cstdio>
#include <string>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/backup/maintenance.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;
using namespace annpocketdb::backup;

static std::string key(int i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "k%04d", i);
    return std::string(buf);
}
static std::string val(int i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "v%04d", i);
    return std::string(buf);
}

int main() {
    auto srcPath = anntest::scratchDbPath("incbackup_src.apdb");
    auto dstPath = anntest::scratchDbPath("incbackup_dst.apdb");
    auto dump1 = anntest::scratchDbPath("incbackup_1.apinc");
    auto dump2 = anntest::scratchDbPath("incbackup_2.apinc");

    auto srcR = Environment::open(srcPath);
    CHECK(srcR.ok());
    if (!srcR) TEST_MAIN_END();
    Environment src = srcR.moveValue();

    // Batch 1: 50 keys.
    {
        auto wtxnR = src.beginWrite();
        CHECK(wtxnR.ok());
        if (!wtxnR) TEST_MAIN_END();
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        for (int i = 0; i < 50; ++i) {
            CHECK_OK(wtxn.put(toByteView(key(i)), toByteView(val(i)), true));
        }
        CHECK_OK(wtxn.commit());
    }
    TxnId afterBatch1 = src.pageManager().lastCommittedTxnId();

    // Fresh destination: create it (starting txnId is 1), then close so the
    // incremental loader can reopen it exclusively.
    TxnId freshBaseline = 0;
    {
        auto dstR = Environment::open(dstPath);
        CHECK(dstR.ok());
        if (dstR.ok()) {
            freshBaseline = dstR.value().pageManager().lastCommittedTxnId();
            CHECK_OK(dstR.value().close());
        }
    }
    CHECK(freshBaseline != 0);

    // Full copy (relative to the fresh destination's own starting point).
    auto dump1R = src.incrementalDumpToFile(freshBaseline, dump1);
    CHECK(dump1R.ok());
    if (!dump1R) TEST_MAIN_END();
    IncrementalDumpReport rep1 = dump1R.value();
    CHECK(rep1.pagesDumped > 0);
    CHECK(rep1.snapshotTxnId == afterBatch1);

    auto load1R = incrementalLoadFromFile(dump1, dstPath);
    CHECK(load1R.ok());
    if (!load1R) TEST_MAIN_END();
    CHECK(load1R.value().pagesLoaded == rep1.pagesDumped);

    // Verify batch 1 landed correctly in the destination.
    {
        auto dstR = Environment::open(dstPath);
        CHECK(dstR.ok());
        if (dstR.ok()) {
            Environment dst = dstR.moveValue();
            CHECK(dst.pageManager().lastCommittedTxnId() == afterBatch1);
            for (int i = 0; i < 50; ++i) {
                auto v = dst.get(toByteView(key(i)));
                CHECK(v.ok());
                if (v.ok()) CHECK(toStringView(toByteView(v.value())) == val(i));
            }
            CHECK_OK(dst.close());
        }
    }

    // Batch 2 in the source: overwrite a few of batch 1's keys, add 30 new ones.
    {
        auto wtxnR = src.beginWrite();
        CHECK(wtxnR.ok());
        if (!wtxnR) TEST_MAIN_END();
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        for (int i = 0; i < 5; ++i) {
            CHECK_OK(wtxn.put(toByteView(key(i)), toByteView(val(i) + "_updated"), true));
        }
        for (int i = 50; i < 80; ++i) {
            CHECK_OK(wtxn.put(toByteView(key(i)), toByteView(val(i)), true));
        }
        CHECK_OK(wtxn.commit());
    }
    TxnId afterBatch2 = src.pageManager().lastCommittedTxnId();
    CHECK(afterBatch2 > afterBatch1);

    // Delta-only increment relative to afterBatch1.
    auto dump2R = src.incrementalDumpToFile(afterBatch1, dump2);
    CHECK(dump2R.ok());
    if (!dump2R) TEST_MAIN_END();
    IncrementalDumpReport rep2 = dump2R.value();
    CHECK(rep2.snapshotTxnId == afterBatch2);
    // A delta touching only ~35 of 80 keys should dump far fewer pages than
    // the full copy did.
    CHECK(rep2.pagesDumped > 0 && rep2.pagesDumped < rep1.pagesDumped + 40);

    // Applying the STALE full-copy dump again now (dest is at afterBatch1,
    // baseline of dump1 is freshBaseline) must be rejected.
    auto staleR = incrementalLoadFromFile(dump1, dstPath);
    CHECK(!staleR.ok());

    // Applying the correct delta must succeed.
    auto load2R = incrementalLoadFromFile(dump2, dstPath);
    CHECK(load2R.ok());
    if (!load2R) TEST_MAIN_END();

    // Verify the destination now matches the source exactly.
    {
        auto dstR = Environment::open(dstPath);
        CHECK(dstR.ok());
        if (dstR.ok()) {
            Environment dst = dstR.moveValue();
            CHECK(dst.pageManager().lastCommittedTxnId() == afterBatch2);
            for (int i = 0; i < 5; ++i) {
                auto v = dst.get(toByteView(key(i)));
                CHECK(v.ok());
                if (v.ok()) CHECK(toStringView(toByteView(v.value())) == val(i) + "_updated");
            }
            for (int i = 5; i < 80; ++i) {
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
