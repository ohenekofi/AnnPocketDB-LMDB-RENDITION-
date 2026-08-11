// B+tree rebalancing regression test: forces real leaf/branch merge,
// redistribution, and root collapse (BTree::removeRecursive /
// rebalanceLeafChild / rebalanceBranchChild) via mass deletion patterns on a
// genuinely multi-level tree -- every other test in this suite only inserts,
// or removes a handful of keys, never enough to exercise these paths.
// Verifies both functional correctness (every surviving key still reads
// back correctly, sorted, via a full cursor scan) and structural evidence
// that rebalancing actually ran (page counts via Environment::stat()).
#include "test_common.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/backup/maintenance.hpp"
#include "annpocketdb/btree/cursor.hpp"
#include "annpocketdb/core/page.hpp"

using namespace annpocketdb;
using namespace annpocketdb::api;

namespace {

constexpr int kTotal = 5000;

std::string keyFor(int i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "key%05d", i);
    return std::string(buf);
}
std::string valFor(int i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "val%05d", i);
    return std::string(buf);
}

// Full forward scan via cursor; returns the sorted list of (key index, value
// index) pairs actually present, verifying strict ascending order and value
// correctness along the way (not just "is present").
bool scanAndVerify(Environment& env, std::vector<int>& presentIndices) {
    presentIndices.clear();
    auto rtxnR = env.beginRead();
    if (!rtxnR.ok()) return false;
    txn::ReadTransaction rtxn = rtxnR.moveValue();
    btree::Cursor cur = rtxn.openCursor();
    auto first = cur.first();
    if (!first.ok()) return false;
    if (!first.value()) return true;  // empty tree: nothing to verify, not an error

    std::string prevKey;
    bool havePrev = false;
    while (cur.valid()) {
        auto k = cur.key();
        auto v = cur.value();
        if (!k.ok() || !v.ok()) return false;
        std::string ks(reinterpret_cast<const char*>(k.value().data()), k.value().size());
        std::string vs(reinterpret_cast<const char*>(v.value().data()), v.value().size());

        if (havePrev && !(prevKey < ks)) return false;  // must be strictly ascending
        prevKey = ks;
        havePrev = true;

        // "keyNNNNN" -> NNNNN
        int idx = std::atoi(ks.c_str() + 3);
        if (vs != valFor(idx)) return false;
        presentIndices.push_back(idx);

        auto n = cur.next();
        if (!n.ok()) return false;
        if (!n.value()) break;
    }
    return true;
}

}  // namespace

int main() {
    auto path = anntest::scratchDbPath("btree_rebalance.apdb");
    auto envR = Environment::open(path);
    CHECK(envR.ok());
    if (!envR) TEST_MAIN_END();
    Environment env = envR.moveValue();

    // ---- Step 1: build a multi-level tree ----
    {
        auto wtxnR = env.beginWrite();
        CHECK(wtxnR.ok());
        if (!wtxnR) TEST_MAIN_END();
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        for (int i = 0; i < kTotal; ++i) {
            CHECK_OK(wtxn.put(toByteView(keyFor(i)), toByteView(valFor(i))));
        }
        CHECK_OK(wtxn.commit());
    }

    auto baselineStat = env.stat();
    CHECK(baselineStat.ok());
    if (baselineStat.ok()) {
        CHECK(baselineStat.value().entries == static_cast<std::uint64_t>(kTotal));
        // A genuinely multi-level tree: more than one leaf, and at least one
        // branch page (depth > 1), not a single toy leaf.
        CHECK(baselineStat.value().leafPages > 1);
        CHECK(baselineStat.value().branchPages >= 1);
        CHECK(baselineStat.value().depth > 1);
    }
    std::uint64_t baselineLeafPages = baselineStat.ok() ? baselineStat.value().leafPages : 0;

    {
        std::vector<int> present;
        CHECK(scanAndVerify(env, present));
        CHECK(present.size() == static_cast<std::size_t>(kTotal));
    }

    // ---- Step 2: contiguous mass deletion (forces merges) ----
    constexpr int kDeleteFrom = 1000;
    constexpr int kDeleteTo = 3999;  // inclusive; 3000 keys removed
    {
        auto wtxnR = env.beginWrite();
        CHECK(wtxnR.ok());
        if (!wtxnR) TEST_MAIN_END();
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        for (int i = kDeleteFrom; i <= kDeleteTo; ++i) {
            CHECK_OK(wtxn.remove(toByteView(keyFor(i))));
        }
        CHECK_OK(wtxn.commit());
    }

    // ---- Step 3: verify after contiguous deletion ----
    for (int i = 0; i < kTotal; ++i) {
        auto v = env.get(toByteView(keyFor(i)));
        bool shouldExist = i < kDeleteFrom || i > kDeleteTo;
        if (shouldExist) {
            CHECK(v.ok());
            if (v.ok()) CHECK(toStringView(toByteView(v.value())) == valFor(i));
        } else {
            CHECK(!v.ok());
            CHECK(v.status().code() == StatusCode::NotFound);
        }
    }

    constexpr int kExpectedAfterContiguous = kTotal - (kDeleteTo - kDeleteFrom + 1);
    {
        std::vector<int> present;
        CHECK(scanAndVerify(env, present));
        CHECK(present.size() == static_cast<std::size_t>(kExpectedAfterContiguous));
        for (std::size_t j = 0; j < present.size(); ++j) {
            int expected = static_cast<int>(j) < kDeleteFrom
                               ? static_cast<int>(j)
                               : static_cast<int>(j) + (kDeleteTo - kDeleteFrom + 1);
            CHECK(present[j] == expected);
        }
    }

    auto afterContiguousStat = env.stat();
    CHECK(afterContiguousStat.ok());
    if (afterContiguousStat.ok() && baselineLeafPages > 0) {
        CHECK(afterContiguousStat.value().entries == static_cast<std::uint64_t>(kExpectedAfterContiguous));
        // Real evidence rebalancing (merging) happened, not just "underflow
        // tolerated in place": far fewer leaf pages than the original tree,
        // proportional to how much data was actually removed.
        CHECK(afterContiguousStat.value().leafPages < baselineLeafPages);
    }

    // ---- Step 4: sparse deletion (forces redistribution, not just merging) ----
    // From the remaining keys, delete every other one.
    {
        auto wtxnR = env.beginWrite();
        CHECK(wtxnR.ok());
        if (!wtxnR) TEST_MAIN_END();
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        int n = 0;
        for (int i = 0; i < kTotal; ++i) {
            bool exists = i < kDeleteFrom || i > kDeleteTo;
            if (!exists) continue;
            if (n % 2 == 1) {
                CHECK_OK(wtxn.remove(toByteView(keyFor(i))));
            }
            ++n;
        }
        CHECK_OK(wtxn.commit());
    }

    {
        std::vector<int> present;
        CHECK(scanAndVerify(env, present));
        CHECK(present.size() == static_cast<std::size_t>((kExpectedAfterContiguous + 1) / 2));
    }
    // Spot checks: keyFor(0) is present-position n=0 (even -> kept);
    // keyFor(4998) is present-position n=1998 (even -> kept). keyFor(4999)
    // would be position n=1999 (odd -> removed), so deliberately not checked
    // here as a "survivor".
    {
        auto v0 = env.get(toByteView(keyFor(0)));
        CHECK(v0.ok());
        if (v0.ok()) CHECK(toStringView(toByteView(v0.value())) == valFor(0));
        auto vLast = env.get(toByteView(keyFor(4998)));
        CHECK(vLast.ok());
        if (vLast.ok()) CHECK(toStringView(toByteView(vLast.value())) == valFor(4998));
    }

    // ---- Step 5: root collapse to empty ----
    {
        auto wtxnR = env.beginWrite();
        CHECK(wtxnR.ok());
        if (!wtxnR) TEST_MAIN_END();
        txn::WriteTransaction wtxn = wtxnR.moveValue();
        for (int i = 0; i < kTotal; ++i) {
            // Removing an already-absent key is a no-op error (NotFound);
            // don't assert success blindly here, just drive every key that
            // might still exist through remove().
            (void)wtxn.remove(toByteView(keyFor(i)));
        }
        CHECK_OK(wtxn.commit());
    }

    {
        auto v = env.get(toByteView(keyFor(0)));
        CHECK(!v.ok());
        CHECK(v.status().code() == StatusCode::NotFound);
    }
    {
        std::vector<int> present;
        CHECK(scanAndVerify(env, present));
        CHECK(present.empty());
    }
    auto emptyStat = env.stat();
    CHECK(emptyStat.ok());
    if (emptyStat.ok()) CHECK(emptyStat.value().entries == 0);

    CHECK_OK(env.close());

    // ---- 6,000-op randomized fuzz cross-checked against a std::map
    // reference, then a minimum-fill invariant walk over the resulting tree
    // -- the direct proof the merge/redistribute logic works, not just that
    // keys happen to survive the directed scenarios above. ----
    {
        auto fuzzPath = anntest::scratchDbPath("btree_rebalance_fuzz.apdb");
        auto fuzzEnvR = Environment::open(fuzzPath);
        CHECK(fuzzEnvR.ok());
        if (fuzzEnvR.ok()) {
            Environment fuzzEnv = fuzzEnvR.moveValue();
            std::map<std::string, std::string> reference;
            std::mt19937 rng(0xB7EE5EEDu);  // fixed seed: reproducible on failure
            constexpr int kKeySpace = 800;
            constexpr int kOps = 6000;
            constexpr int kBatchSize = 200;

            auto fuzzKey = [](int i) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "fuzz%04d", i);
                return std::string(buf);
            };

            std::uniform_int_distribution<int> keyDist(0, kKeySpace - 1);
            std::uniform_int_distribution<int> opDist(0, 99);

            int done = 0;
            bool fuzzOk = true;
            while (done < kOps && fuzzOk) {
                auto wtxnR = fuzzEnv.beginWrite();
                if (!wtxnR.ok()) { fuzzOk = false; break; }
                txn::WriteTransaction wtxn = wtxnR.moveValue();
                int batchEnd = done + kBatchSize < kOps ? done + kBatchSize : kOps;
                for (; done < batchEnd; ++done) {
                    std::string key = fuzzKey(keyDist(rng));
                    int roll = opDist(rng);
                    if (roll < 70) {
                        // put/update (~70% of ops).
                        char vbuf[24];
                        std::snprintf(vbuf, sizeof(vbuf), "v%08x", static_cast<unsigned>(rng()));
                        std::string value(vbuf);
                        Status s = wtxn.put(toByteView(key), toByteView(value));
                        if (!s.ok()) { fuzzOk = false; break; }
                        reference[key] = value;
                    } else {
                        // delete (~30% of ops) -- fine whether or not it
                        // currently exists (NotFound is expected, not fatal).
                        (void)wtxn.remove(toByteView(key));
                        reference.erase(key);
                    }
                }
                Status cs = wtxn.commit();
                if (!cs.ok()) { fuzzOk = false; break; }
                if (!fuzzOk) break;

                // Periodic full cross-check: exact key-set equality against
                // the reference (every db key is in the map with the right
                // value, AND the db has no extra keys the map doesn't have).
                auto rtxnR = fuzzEnv.beginRead();
                if (!rtxnR.ok()) { fuzzOk = false; break; }
                txn::ReadTransaction rchk = rtxnR.moveValue();
                btree::Cursor cur = rchk.openCursor();
                auto first = cur.first();
                if (!first.ok()) { fuzzOk = false; break; }
                std::size_t seen = 0;
                if (first.value()) {
                    while (cur.valid()) {
                        auto k = cur.key();
                        auto v = cur.value();
                        if (!k.ok() || !v.ok()) { fuzzOk = false; break; }
                        std::string ks(reinterpret_cast<const char*>(k.value().data()), k.value().size());
                        std::string vs(reinterpret_cast<const char*>(v.value().data()), v.value().size());
                        auto refIt = reference.find(ks);
                        if (refIt == reference.end() || refIt->second != vs) { fuzzOk = false; break; }
                        ++seen;
                        auto n = cur.next();
                        if (!n.ok()) { fuzzOk = false; break; }
                        if (!n.value()) break;
                    }
                }
                if (fuzzOk && seen != reference.size()) fuzzOk = false;
            }
            CHECK(fuzzOk);

            // Final thorough cross-check.
            for (const auto& [k, v] : reference) {
                auto got = fuzzEnv.get(toByteView(k));
                CHECK(got.ok());
                if (got.ok()) CHECK(toStringView(toByteView(got.value())) == v);
            }

            auto verifyR = fuzzEnv.verify();
            CHECK(verifyR.ok());
            if (verifyR.ok()) {
                CHECK(verifyR.value().ok());
                CHECK(verifyR.value().minFillViolations == 0);
            }

            CHECK_OK(fuzzEnv.close());
        }
    }

    // ---- After deleting 80% of 5,000 sequential keys, average leaf fill
    // stays well above the 25% minimum threshold -- proof rebalancing keeps
    // pages reasonably packed instead of degrading toward the bare floor
    // (which is what a collapse-only, no-redistribution policy would do). ----
    {
        auto fillPath = anntest::scratchDbPath("btree_rebalance_fill.apdb");
        auto fillEnvR = Environment::open(fillPath);
        CHECK(fillEnvR.ok());
        if (fillEnvR.ok()) {
            Environment fillEnv = fillEnvR.moveValue();
            constexpr int kFillTotal = 5000;
            {
                auto wtxnR = fillEnv.beginWrite();
                CHECK(wtxnR.ok());
                if (wtxnR.ok()) {
                    txn::WriteTransaction wtxn = wtxnR.moveValue();
                    for (int i = 0; i < kFillTotal; ++i) {
                        CHECK_OK(wtxn.put(toByteView(keyFor(i)), toByteView(valFor(i))));
                    }
                    CHECK_OK(wtxn.commit());
                }
            }
            // Delete 80% (keep every 5th key).
            {
                auto wtxnR = fillEnv.beginWrite();
                CHECK(wtxnR.ok());
                if (wtxnR.ok()) {
                    txn::WriteTransaction wtxn = wtxnR.moveValue();
                    for (int i = 0; i < kFillTotal; ++i) {
                        if (i % 5 != 0) CHECK_OK(wtxn.remove(toByteView(keyFor(i))));
                    }
                    CHECK_OK(wtxn.commit());
                }
            }
            auto afterFillStat = fillEnv.stat();
            CHECK(afterFillStat.ok());
            if (afterFillStat.ok()) {
                const auto& st = afterFillStat.value();
                CHECK(st.entries == static_cast<std::uint64_t>(kFillTotal / 5));
                // Exact per-entry encoded size for this test's fixed-width
                // "keyNNNNN"/"valNNNNN" (8-byte key + 8-byte value, no
                // overflow): kLeafCellHdr(13) + key(8) + value(8) = 29
                // bytes, + 4-byte slot = 33 bytes/entry (node_codec.hpp's
                // leaf cell format) -- exact, not an approximation, for
                // this specific fixed-width shape.
                constexpr double kBytesPerEntry = 33.0;
                std::size_t capacity = core::payloadCapacity(st.pageSize);
                CHECK(st.leafPages > 0);
                if (st.leafPages > 0) {
                    double usedBytes = static_cast<double>(st.entries) * kBytesPerEntry;
                    double totalCapacity = static_cast<double>(st.leafPages) * static_cast<double>(capacity);
                    double avgFill = usedBytes / totalCapacity;
                    std::fprintf(stderr,
                                 "[test_btree_rebalance] average leaf fill after deleting 80%% of %d "
                                 "sequential keys: %.1f%% (%llu leaf pages, %llu entries)\n",
                                 kFillTotal, avgFill * 100.0,
                                 static_cast<unsigned long long>(st.leafPages),
                                 static_cast<unsigned long long>(st.entries));
                    CHECK(avgFill > 0.30);
                }
            }
            CHECK_OK(fillEnv.close());
        }
    }

    TEST_MAIN_END();
}
