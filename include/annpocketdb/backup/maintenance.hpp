#pragma once
//
// AnnPocketDb :: backup/maintenance.hpp
//
// Online verify / repair and offline dump / load.
//
// Differentiators vs LMDB / libmdbx:
//   - verify() runs *in-process* against a live Environment (or a
//     read-only open of a file) and walks every reachable page checking
//     CRC, slot bounds, sort order, and chain termination.
//   - repair() salvages every key/value that still materializes into a
//     *fresh* database file — the "fork that heals itself" path.
//   - dump/load are the classic offline tools (portable archive format).
//
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

namespace annpocketdb {
namespace api {
class Environment;  // defined in api/environment.hpp
struct EnvironmentOptions;
}  // namespace api

namespace backup {

// ---------------------------------------------------------------------------
// Verify
// ---------------------------------------------------------------------------

struct VerifyIssue {
    PageId pageId = kInvalidPageId;
    std::string message;
};

struct VerifyReport {
    std::uint64_t pagesScanned = 0;
    std::uint64_t leafEntries = 0;
    std::uint64_t branchEntries = 0;
    std::uint64_t overflowPages = 0;
    std::uint64_t freeListPages = 0;
    std::uint64_t treeDepthMax = 0;
    // Non-root leaf/branch pages whose encoded entry size fell below the
    // BTree's own minimum-fill threshold (payloadCapacity/4 -- see
    // BTree::minFillThreshold). The root is exempt (a tree with few enough
    // entries to fit in one page is never required to "merge" further).
    // A non-zero count here is the structural proof that
    // rebalanceLeafChild/rebalanceBranchChild actually ran correctly after
    // deletions, not just that keys still happen to be readable.
    std::uint64_t minFillViolations = 0;
    std::vector<VerifyIssue> issues;

    bool ok() const { return issues.empty(); }
};

// Walk the live environment: meta, master B+tree, free-list chain, and
// (best-effort) every overflow chain reachable from leaf entries. Also
// checks the minimum-fill invariant on every non-root leaf/branch node
// (see VerifyReport::minFillViolations).
// Does not mutate the database. Safe under concurrent readers; the caller
// should not hold a WriteTransaction (verify takes a read snapshot).
Result<VerifyReport> verify(api::Environment& env);

// Open `path` read-only solely to verify, then close. Convenience for
// offline CLI use without holding an Environment in the caller.
Result<VerifyReport> verifyFile(const std::filesystem::path& path);

// ---------------------------------------------------------------------------
// Repair (salvage into a fresh file)
// ---------------------------------------------------------------------------

struct RepairOptions {
    // Destination path must not already exist (or is truncated if force).
    bool forceOverwrite = false;
    // Copy pageSize / maxDatabaseSize from the source when true.
    bool copyLayout = true;
    // Stop after this many consecutive unreadable entries (0 = no limit).
    std::uint64_t maxConsecutiveFailures = 0;
    // Optional progress callback (entries salvaged so far).
    std::function<void(std::uint64_t salvaged, std::uint64_t skipped)> onProgress;
};

struct RepairReport {
    std::uint64_t salvaged = 0;
    std::uint64_t skipped = 0;
    std::vector<std::string> notes;
    VerifyReport sourceVerify;  // pre-repair scan (may itself be partial)
};

// Open `source` read-only, create a fresh DB at `dest`, and copy every
// key/value that can still be read. Corrupted subtrees are skipped with
// a note rather than aborting the whole salvage.
Result<RepairReport> repair(const std::filesystem::path& source,
                            const std::filesystem::path& dest,
                            const RepairOptions& options = {});

// ---------------------------------------------------------------------------
// Offline dump / load (portable archive)
// ---------------------------------------------------------------------------
//
// Wire format (little-endian):
//   magic[8]     = "APDUMP01"
//   version u32  = 1
//   flags   u32  = 0
//   reserved u64 = 0
//   repeated:
//     keyLen u32   (0 => end of stream)
//     key bytes
//     valueLen u32
//     value bytes
//

struct DumpOptions {
    std::function<void(std::uint64_t count)> onProgress;
};

struct DumpReport {
    std::uint64_t entries = 0;
};

struct LoadOptions {
    api::EnvironmentOptions* envOptions = nullptr;  // nullptr => defaults
    bool forceOverwrite = false;
    std::function<void(std::uint64_t count)> onProgress;
};

struct LoadReport {
    std::uint64_t entries = 0;
};

Result<DumpReport> dumpToFile(api::Environment& env,
                              const std::filesystem::path& outPath,
                              const DumpOptions& options = {});

Result<DumpReport> dumpFile(const std::filesystem::path& dbPath,
                            const std::filesystem::path& outPath,
                            const DumpOptions& options = {});

Result<LoadReport> loadFromFile(const std::filesystem::path& dumpPath,
                                const std::filesystem::path& dbPath,
                                const LoadOptions& options = {});

// ---------------------------------------------------------------------------
// Compacting copy (LMDB's Environment.copy(path, compact=True))
// ---------------------------------------------------------------------------
//
// Produces a fresh, defragmented copy of `source`'s default keyspace at
// `destPath`: dumps every live key/value via a logical walk and reloads them
// into a brand-new destination file. Free/reclaimed pages and whatever
// fragmentation the source accumulated over its lifetime are never carried
// over, since the destination is built fresh from only the live records --
// the same net effect as LMDB's page-renumbering compact copy, achieved by
// rebuilding rather than renumbering in place. Round-trips through a
// temporary dump file next to `destPath` (best-effort removed afterward)
// rather than a separate parallel read/write path, so it's exactly as
// correct as the already-independently-tested dumpToFile/loadFromFile pair.
// `destPath` must not already exist unless `forceOverwrite` is set.
//
// Scope note: like dumpToFile/loadFromFile, this covers the default
// keyspace only, not named sub-databases' own trees (see
// incrementalDumpToFile's doc comment for the same, pre-existing limit).
struct CompactCopyOptions {
    std::function<void(std::uint64_t entries)> onProgress;
};

struct CompactCopyReport {
    std::uint64_t entries = 0;
};

Result<CompactCopyReport> compactCopy(api::Environment& source, const std::filesystem::path& destPath,
                                      bool forceOverwrite = false,
                                      const CompactCopyOptions& options = {});

// ---------------------------------------------------------------------------
// Incremental / delta backup (physical page-level, LMDB mdb_env_copy2/
// loadfd-style)
// ---------------------------------------------------------------------------
//
// Unlike dumpToFile/loadFromFile above (a logical, key/value-level, full
// snapshot), this pair works at the *physical page* level: incrementalDump
// walks the master keyspace tree and free list from a live Environment and
// writes out only the pages whose PageHeader::txnId is greater than
// `baselineTxnId` -- COW guarantees a page's txnId bounds every descendant's
// txnId, so a page at or below the baseline (and everything under it) can be
// pruned without reading it. Reachable DUPSORT escalation sub-trees and
// overflow chains are included; named sub-databases' own trees are not (the
// same scope dumpToFile/verify already have -- a pre-existing limitation of
// this codebase's offline tooling, not one introduced here).
//
// incrementalLoadFromFile replays the dumped page images verbatim into an
// existing target file that must already sit at exactly `baselineTxnId`
// (checked strictly against the target's current committed txnId) --
// increments must be applied in order over their exact prior baseline, the
// same contract as LMDB's mdb_env_copy2/loadfd pair. This is not a general
// merge tool. To take a full copy into a brand-new destination, open an
// empty Environment at the same pageSize (its starting txnId is 1) and pass
// baselineTxnId = 1 to incrementalDumpToFile.
//
// Wire format (little-endian):
//   magic[8]           = "APIDMP01"
//   version u32        = 1
//   pageSize u32
//   baselineTxnId u64
//   snapshotTxnId u64   (the txnId this dump was taken at; becomes the next
//                        increment's baselineTxnId once applied)
//   masterRootPageId u64
//   freeListRootPageId u64
//   repeated:
//     pageId u64          (all-ones => end of stream)
//     raw page bytes[pageSize]   (verbatim PageHeader + payload)
//

struct IncrementalDumpOptions {
    std::function<void(std::uint64_t pagesDumped)> onProgress;
};

struct IncrementalDumpReport {
    std::uint64_t pagesDumped = 0;
    TxnId baselineTxnId = 0;
    TxnId snapshotTxnId = 0;  // pass as the next call's baselineTxnId
};

Result<IncrementalDumpReport> incrementalDumpToFile(api::Environment& env, TxnId baselineTxnId,
                                                    const std::filesystem::path& outPath,
                                                    const IncrementalDumpOptions& options = {});

struct IncrementalLoadOptions {
    // Forwarded to Environment::open() for the destination (pageSize is
    // always overridden from the dump header regardless). In particular,
    // set enableEncryption/encryptionKey here to match the destination's
    // encryption settings -- required both so an already-encrypted
    // destination (from a prior increment) can be reopened for decryption,
    // and so this increment's pages get correctly (re-)encrypted into it.
    // nullptr => defaults (no encryption).
    api::EnvironmentOptions* envOptions = nullptr;
    std::function<void(std::uint64_t pagesLoaded)> onProgress;
};

struct IncrementalLoadReport {
    std::uint64_t pagesLoaded = 0;
};

Result<IncrementalLoadReport> incrementalLoadFromFile(const std::filesystem::path& dumpPath,
                                                      const std::filesystem::path& dbPath,
                                                      const IncrementalLoadOptions& options = {});

}  // namespace backup
}  // namespace annpocketdb
