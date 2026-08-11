#include "annpocketdb/backup/maintenance.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_set>

#include "annpocketdb/api/environment.hpp"
#include "annpocketdb/btree/btree.hpp"
#include "annpocketdb/btree/cursor.hpp"
#include "annpocketdb/btree/node_codec.hpp"
#include "annpocketdb/core/page.hpp"
#include "annpocketdb/core/page_manager.hpp"
#include "annpocketdb/util/bounded_walk.hpp"

namespace annpocketdb::backup {

namespace {

constexpr char kDumpMagic[8] = {'A', 'P', 'D', 'U', 'M', 'P', '0', '1'};
constexpr std::uint32_t kDumpVersion = 1;

void addIssue(VerifyReport& r, PageId id, std::string msg) {
    r.issues.push_back(VerifyIssue{id, std::move(msg)});
}

Status verifyPageHeader(core::PageManager& pm, PageId id, const core::PageHeader* h,
                        VerifyReport& report) {
    if (h->pageId != id) {
        addIssue(report, id, "pageId field does not match address");
        return Status::Corruption("pageId mismatch");
    }
    if (h->payloadSize > core::payloadCapacity(pm.pageSize())) {
        addIssue(report, id, "payloadSize exceeds capacity");
        return Status::Corruption("payloadSize");
    }
    return Status::OK();
}

Status verifyOverflowChain(core::PageManager& pm, PageId first, std::uint64_t claimedLen,
                           VerifyReport& report) {
    PageId cur = first;
    std::uint64_t seen = 0;
    BoundedWalk walk(std::max<std::uint64_t>(pm.pageCount(), 1) + 2);
    std::unordered_set<PageId> visited;
    while (cur != kInvalidPageId) {
        Status bs = walk.step("verify overflow");
        if (!bs.ok()) {
            addIssue(report, cur, bs.message());
            return bs;
        }
        if (!visited.insert(cur).second) {
            addIssue(report, cur, "overflow chain cycle");
            return Status::Corruption("overflow cycle");
        }
        auto pr = pm.getPage(cur);
        if (!pr) {
            addIssue(report, cur, "overflow page unreadable: " + pr.status().message());
            return pr.status();
        }
        const core::PageHeader* h = pr.value();
        Status hs = verifyPageHeader(pm, cur, h, report);
        if (!hs.ok()) return hs;
        if (h->type != core::PageType::Overflow) {
            addIssue(report, cur, "expected Overflow page type");
            return Status::Corruption("bad overflow type");
        }
        ++report.overflowPages;
        seen += h->payloadSize;
        cur = h->overflowNext;
    }
    if (claimedLen > 0 && seen < claimedLen) {
        addIssue(report, first, "overflow chain shorter than claimed totalLen");
    }
    return Status::OK();
}

// Matches BTree::minFillThreshold() exactly (payloadCapacity/4) -- kept as
// a free function here since that method is private to BTree and this is
// just arithmetic on values verify() already has (pageSize).
std::size_t minFillBytes(const core::PageManager& pm) {
    return core::payloadCapacity(pm.pageSize()) / 4;
}

Status verifyLeaf(core::PageManager& pm, PageId id, const core::PageHeader* h,
                  VerifyReport& report, bool isRoot) {
    Status hs = verifyPageHeader(pm, id, h, report);
    if (!hs.ok()) return hs;
#if defined(ANNPOCKETDB_PARANOID)
    // Full structural check already available when built paranoid.
#endif
    btree::LeafPageView view(h);
    ByteView prev;
    bool havePrev = false;
    for (std::uint16_t i = 0; i < view.count(); ++i) {
        ByteView k = view.keyAt(i);
        if (havePrev && compareBytes(prev, k) >= 0) {
            addIssue(report, id, "leaf keys out of order at index " + std::to_string(i));
        }
        prev = k;
        havePrev = true;
        ++report.leafEntries;
        auto entry = view.entryAt(i);
        if (entry.isOverflow) {
            Status os = verifyOverflowChain(pm, entry.overflowFirstPage,
                                            entry.overflowTotalLen, report);
            (void)os;  // issues already recorded
        }
    }
    if (!isRoot) {
        std::size_t encoded = btree::encodedLeafSize(view.decodeAll());
        std::size_t minFill = minFillBytes(pm);
        if (encoded < minFill) {
            ++report.minFillViolations;
            addIssue(report, id,
                     "leaf page underflows minimum fill threshold (" + std::to_string(encoded) +
                         " < " + std::to_string(minFill) + " bytes)");
        }
    }
    return Status::OK();
}

Status verifyBranch(core::PageManager& pm, PageId id, const core::PageHeader* h,
                    VerifyReport& report, bool isRoot) {
    Status hs = verifyPageHeader(pm, id, h, report);
    if (!hs.ok()) return hs;
    btree::BranchPageView view(h);
    if (view.count() == 0) {
        addIssue(report, id, "branch page with zero children");
        return Status::Corruption("empty branch");
    }
    ByteView prev;
    bool havePrev = false;
    for (std::uint16_t i = 0; i < view.count(); ++i) {
        ++report.branchEntries;
        if (i > 0) {
            ByteView k = view.keyAt(i);
            if (havePrev && compareBytes(prev, k) >= 0) {
                addIssue(report, id, "branch keys out of order at index " + std::to_string(i));
            }
            prev = k;
            havePrev = true;
        }
        PageId child = view.childAt(i);
        if (child == kInvalidPageId || child >= pm.pageCount()) {
            addIssue(report, id, "branch child out of range at index " + std::to_string(i));
        }
    }
    if (!isRoot) {
        std::size_t encoded = btree::encodedBranchSize(view.decodeAll());
        std::size_t minFill = minFillBytes(pm);
        if (encoded < minFill) {
            ++report.minFillViolations;
            addIssue(report, id,
                     "branch page underflows minimum fill threshold (" + std::to_string(encoded) +
                         " < " + std::to_string(minFill) + " bytes)");
        }
    }
    return Status::OK();
}

Status verifyTree(core::PageManager& pm, PageId root, VerifyReport& report,
                  std::unordered_set<PageId>& visited) {
    if (root == kInvalidPageId) return Status::OK();

    struct Frame {
        PageId id;
        std::uint32_t depth;
    };
    std::vector<Frame> stack;
    stack.push_back({root, 0});

    while (!stack.empty()) {
        Frame f = stack.back();
        stack.pop_back();
        if (f.depth > kMaxTreeDepth) {
            addIssue(report, f.id, "tree depth exceeded bound");
            continue;
        }
        if (report.treeDepthMax < f.depth) report.treeDepthMax = f.depth;
        if (!visited.insert(f.id).second) {
            addIssue(report, f.id, "page reachable via multiple parents (or cycle)");
            continue;
        }
        auto pr = pm.getPage(f.id);
        if (!pr) {
            addIssue(report, f.id, "unreadable page: " + pr.status().message());
            continue;
        }
        const core::PageHeader* h = pr.value();
        ++report.pagesScanned;
        bool isRoot = (f.depth == 0);
        if (h->type == core::PageType::Leaf) {
            verifyLeaf(pm, f.id, h, report, isRoot);
        } else if (h->type == core::PageType::Branch) {
            verifyBranch(pm, f.id, h, report, isRoot);
            btree::BranchPageView view(h);
            // Push children in reverse so left-to-right is processed first.
            for (std::uint16_t i = view.count(); i-- > 0;) {
                stack.push_back({view.childAt(i), f.depth + 1});
            }
        } else {
            addIssue(report, f.id, "unexpected page type in tree");
        }
    }
    return Status::OK();
}

Status verifyFreeList(core::PageManager& pm, PageId head, VerifyReport& report,
                      std::unordered_set<PageId>& visited) {
    PageId cur = head;
    BoundedWalk walk(kMaxChainStepsFallback);
    while (cur != kInvalidPageId) {
        Status bs = walk.step("verify free-list");
        if (!bs.ok()) {
            addIssue(report, cur, bs.message());
            return bs;
        }
        if (!visited.insert(cur).second) {
            addIssue(report, cur, "free-list cycle");
            return Status::Corruption("free-list cycle");
        }
        auto pr = pm.getPage(cur);
        if (!pr) {
            addIssue(report, cur, "free-list page unreadable: " + pr.status().message());
            return pr.status();
        }
        const core::PageHeader* h = pr.value();
        ++report.pagesScanned;
        ++report.freeListPages;
        if (h->type != core::PageType::Free) {
            addIssue(report, cur, "free-list page has wrong type");
        }
        cur = h->overflowNext;
    }
    return Status::OK();
}

Result<VerifyReport> verifyEnv(api::Environment& env) {
    VerifyReport report;
    core::PageManager& pm = env.pageManager();
    core::Snapshot snap = pm.currentSnapshot();
    std::unordered_set<PageId> visited;

    // Meta pages 0 and 1 are always present; soft-check only.
    for (PageId metaId : {PageId{0}, PageId{1}}) {
        auto pr = pm.getPage(metaId);
        if (!pr) {
            addIssue(report, metaId, "meta page unreadable: " + pr.status().message());
        } else {
            ++report.pagesScanned;
            visited.insert(metaId);
        }
    }

    verifyTree(pm, snap.masterRootPageId, report, visited);
    verifyFreeList(pm, snap.freeListRootPageId, report, visited);
    return Result<VerifyReport>(std::move(report));
}

// Write little-endian helpers for the dump format.
void writeU32(std::ostream& out, std::uint32_t v) {
    char b[4];
    b[0] = static_cast<char>(v & 0xff);
    b[1] = static_cast<char>((v >> 8) & 0xff);
    b[2] = static_cast<char>((v >> 16) & 0xff);
    b[3] = static_cast<char>((v >> 24) & 0xff);
    out.write(b, 4);
}

void writeU64(std::ostream& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        char c = static_cast<char>((v >> (8 * i)) & 0xff);
        out.write(&c, 1);
    }
}

bool readU32(std::istream& in, std::uint32_t& v) {
    unsigned char b[4];
    if (!in.read(reinterpret_cast<char*>(b), 4)) return false;
    v = static_cast<std::uint32_t>(b[0]) |
        (static_cast<std::uint32_t>(b[1]) << 8) |
        (static_cast<std::uint32_t>(b[2]) << 16) |
        (static_cast<std::uint32_t>(b[3]) << 24);
    return true;
}

bool readExact(std::istream& in, void* dst, std::size_t n) {
    return static_cast<bool>(in.read(static_cast<char*>(dst), static_cast<std::streamsize>(n)));
}

constexpr char kIncMagic[8] = {'A', 'P', 'I', 'D', 'M', 'P', '0', '1'};
constexpr std::uint32_t kIncVersion = 1;

// writeU64 already defined above (used by dumpToFile's reserved field).
bool readU64(std::istream& in, std::uint64_t& v) {
    unsigned char b[8];
    if (!in.read(reinterpret_cast<char*>(b), 8)) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(b[i]) << (8 * i);
    return true;
}

// Physically walks every page reachable from `roots` (master tree root and
// free-list root), pruning any page (and everything beneath it) whose
// txnId is already <= baselineTxnId. Dumps {pageId, raw pageSize-byte page
// image} for everything else visited. Handles Branch (children), Leaf
// (DUPSORT sub-tree roots + overflow chains), Overflow, and Free pages
// uniformly via one work stack -- a sub-tree root is just another tree, and
// Overflow/Free pages are singly-linked via PageHeader::overflowNext.
Status incrementalWalkAndDump(core::PageManager& pm, const std::vector<PageId>& roots,
                              TxnId baselineTxnId, std::ofstream& out,
                              std::uint64_t& pagesDumped,
                              const IncrementalDumpOptions& options) {
    std::vector<PageId> stack(roots.begin(), roots.end());
    std::unordered_set<PageId> visited;
    BoundedWalk walk(pm.pageCount() + 64);
    while (!stack.empty()) {
        PageId id = stack.back();
        stack.pop_back();
        if (id == kInvalidPageId) continue;
        if (!visited.insert(id).second) continue;
        Status ws = walk.step("incremental dump page walk");
        if (!ws.ok()) return ws;
        auto pr = pm.getPage(id);
        if (!pr) return pr.status();
        const core::PageHeader* h = pr.value();
        if (h->txnId <= baselineTxnId) continue;  // unchanged since baseline; prune subtree

        writeU64(out, static_cast<std::uint64_t>(id));
        out.write(reinterpret_cast<const char*>(h), static_cast<std::streamsize>(pm.pageSize()));
        ++pagesDumped;
        if (options.onProgress) options.onProgress(pagesDumped);

        if (h->type == core::PageType::Branch) {
            btree::BranchPageView v(h);
            for (std::uint16_t i = v.count(); i-- > 0;) stack.push_back(v.childAt(i));
        } else if (h->type == core::PageType::Leaf) {
            btree::LeafPageView v(h);
            for (std::uint16_t i = 0; i < v.count(); ++i) {
                if (v.isOverflowAt(i)) stack.push_back(v.overflowFirstPageAt(i));
                else if (v.isSubTreeAt(i)) stack.push_back(v.subTreeRootAt(i));
            }
        } else if (h->type == core::PageType::Overflow || h->type == core::PageType::Free) {
            if (h->overflowNext != kInvalidPageId) stack.push_back(h->overflowNext);
        }
    }
    return Status::OK();
}

}  // namespace

Result<VerifyReport> verify(api::Environment& env) {
    return verifyEnv(env);
}

Result<VerifyReport> verifyFile(const std::filesystem::path& path) {
    api::EnvironmentOptions opts;
    opts.readOnly = true;
    auto envR = api::Environment::open(path, opts);
    if (!envR) return envR.status();
    return verifyEnv(envR.value());
}

Result<DumpReport> dumpToFile(api::Environment& env, const std::filesystem::path& outPath,
                              const DumpOptions& options) {
    auto txnR = env.beginRead();
    if (!txnR) return txnR.status();
    txn::ReadTransaction& txn = txnR.value();

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out) return Status::IOError("cannot open dump output: " + outPath.string());

    out.write(kDumpMagic, 8);
    writeU32(out, kDumpVersion);
    writeU32(out, 0);  // flags
    writeU64(out, 0);  // reserved

    DumpReport report;
    btree::Cursor cur = txn.openCursor();
    auto first = cur.first();
    if (!first) return first.status();
    if (!first.value()) {
        writeU32(out, 0);  // end
        return Result<DumpReport>(report);
    }

    while (cur.valid()) {
        auto kR = cur.key();
        if (!kR) return kR.status();
        auto vR = cur.value();
        if (!vR) {
            // Skip unreadable values; still count as progress for repair-like dump.
            auto n = cur.next();
            if (!n) return n.status();
            if (!n.value()) break;
            continue;
        }
        ByteView key = kR.value();
        ByteBuffer val = vR.value();
        if (key.size() > 0xffffffffu || val.size() > 0xffffffffu) {
            return Status::InvalidArgument("entry too large for dump format");
        }
        writeU32(out, static_cast<std::uint32_t>(key.size()));
        out.write(reinterpret_cast<const char*>(key.data()),
                  static_cast<std::streamsize>(key.size()));
        writeU32(out, static_cast<std::uint32_t>(val.size()));
        out.write(reinterpret_cast<const char*>(val.data()),
                  static_cast<std::streamsize>(val.size()));
        ++report.entries;
        if (options.onProgress) options.onProgress(report.entries);

        auto n = cur.next();
        if (!n) return n.status();
        if (!n.value()) break;
    }
    writeU32(out, 0);  // end sentinel
    if (!out) return Status::IOError("write failed during dump");
    return Result<DumpReport>(report);
}

Result<DumpReport> dumpFile(const std::filesystem::path& dbPath,
                            const std::filesystem::path& outPath,
                            const DumpOptions& options) {
    api::EnvironmentOptions opts;
    opts.readOnly = true;
    auto envR = api::Environment::open(dbPath, opts);
    if (!envR) return envR.status();
    return dumpToFile(envR.value(), outPath, options);
}

Result<LoadReport> loadFromFile(const std::filesystem::path& dumpPath,
                                const std::filesystem::path& dbPath,
                                const LoadOptions& options) {
    if (std::filesystem::exists(dbPath) && !options.forceOverwrite) {
        return Status::InvalidArgument("destination exists; pass forceOverwrite");
    }
    if (std::filesystem::exists(dbPath) && options.forceOverwrite) {
        std::error_code ec;
        std::filesystem::remove(dbPath, ec);
        std::filesystem::remove(dbPath.string() + ".lock", ec);
    }

    std::ifstream in(dumpPath, std::ios::binary);
    if (!in) return Status::IOError("cannot open dump: " + dumpPath.string());

    char magic[8];
    if (!readExact(in, magic, 8) || std::memcmp(magic, kDumpMagic, 8) != 0) {
        return Status::Corruption("bad dump magic");
    }
    std::uint32_t version = 0, flags = 0;
    if (!readU32(in, version) || !readU32(in, flags)) {
        return Status::Corruption("truncated dump header");
    }
    if (version != kDumpVersion) {
        return Status::Incompatible("unsupported dump version");
    }
    // reserved u64
    char reserved[8];
    if (!readExact(in, reserved, 8)) return Status::Corruption("truncated dump header");

    api::EnvironmentOptions envOpts;
    if (options.envOptions) envOpts = *options.envOptions;
    auto envR = api::Environment::open(dbPath, envOpts);
    if (!envR) return envR.status();
    api::Environment& env = envR.value();

    LoadReport report;
    // Batch puts into write transactions of reasonable size.
    constexpr std::uint64_t kBatch = 512;
    auto txnR = env.beginWrite();
    if (!txnR) return txnR.status();
    txn::WriteTransaction wtxn = txnR.moveValue();

    while (true) {
        std::uint32_t keyLen = 0;
        if (!readU32(in, keyLen)) return Status::Corruption("truncated dump");
        if (keyLen == 0) break;  // end
        ByteBuffer key(keyLen);
        if (!readExact(in, key.data(), keyLen)) return Status::Corruption("truncated key");
        std::uint32_t valLen = 0;
        if (!readU32(in, valLen)) return Status::Corruption("truncated value length");
        ByteBuffer val(valLen);
        if (valLen && !readExact(in, val.data(), valLen)) {
            return Status::Corruption("truncated value");
        }
        Status ps = wtxn.put(toByteView(key), toByteView(val), true);
        if (!ps.ok()) return ps;
        ++report.entries;
        if (options.onProgress) options.onProgress(report.entries);
        if (report.entries % kBatch == 0) {
            Status cs = wtxn.commit();
            if (!cs.ok()) return cs;
            auto next = env.beginWrite();
            if (!next) return next.status();
            wtxn = next.moveValue();
        }
    }
    Status cs = wtxn.commit();
    if (!cs.ok()) return cs;
    return Result<LoadReport>(report);
}

Result<IncrementalDumpReport> incrementalDumpToFile(api::Environment& env, TxnId baselineTxnId,
                                                    const std::filesystem::path& outPath,
                                                    const IncrementalDumpOptions& options) {
    core::PageManager& pm = env.pageManager();
    core::Snapshot snap = pm.currentSnapshot();
    // Pin this snapshot as a live reader for the duration of the walk so the
    // writer cannot reclaim (retire-and-reuse) any page we might still visit.
    core::ReaderHandle handle = pm.registerReader(snap.txnId);
    if (handle == core::kInvalidReaderHandle) {
        return Status::ReadersFull("reader table full; too many concurrent readers across processes");
    }
    struct ReaderGuard {
        core::PageManager& pm;
        core::ReaderHandle handle;
        ~ReaderGuard() { pm.unregisterReader(handle); }
    } guard{pm, handle};

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out) return Status::IOError("cannot open incremental dump output: " + outPath.string());

    out.write(kIncMagic, 8);
    writeU32(out, kIncVersion);
    writeU32(out, pm.pageSize());
    writeU64(out, static_cast<std::uint64_t>(baselineTxnId));
    writeU64(out, static_cast<std::uint64_t>(snap.txnId));
    writeU64(out, static_cast<std::uint64_t>(snap.masterRootPageId));
    writeU64(out, static_cast<std::uint64_t>(snap.freeListRootPageId));

    IncrementalDumpReport report;
    report.baselineTxnId = baselineTxnId;
    report.snapshotTxnId = snap.txnId;

    std::vector<PageId> roots;
    if (snap.masterRootPageId != kInvalidPageId) roots.push_back(snap.masterRootPageId);
    if (snap.freeListRootPageId != kInvalidPageId) roots.push_back(snap.freeListRootPageId);

    Status ws = incrementalWalkAndDump(pm, roots, baselineTxnId, out, report.pagesDumped, options);
    if (!ws.ok()) return ws;

    writeU64(out, static_cast<std::uint64_t>(kInvalidPageId));  // end sentinel
    if (!out) return Status::IOError("write failed during incremental dump");
    return Result<IncrementalDumpReport>(report);
}

Result<IncrementalLoadReport> incrementalLoadFromFile(const std::filesystem::path& dumpPath,
                                                      const std::filesystem::path& dbPath,
                                                      const IncrementalLoadOptions& options) {
    std::ifstream in(dumpPath, std::ios::binary);
    if (!in) return Status::IOError("cannot open incremental dump: " + dumpPath.string());

    char magic[8];
    if (!readExact(in, magic, 8) || std::memcmp(magic, kIncMagic, 8) != 0) {
        return Status::Corruption("bad incremental dump magic");
    }
    std::uint32_t version = 0, pageSize = 0;
    if (!readU32(in, version)) return Status::Corruption("truncated incremental dump header");
    if (version != kIncVersion) return Status::Incompatible("unsupported incremental dump version");
    if (!readU32(in, pageSize)) return Status::Corruption("truncated incremental dump header");
    std::uint64_t baselineTxnId = 0, snapshotTxnId = 0, masterRootRaw = 0, freeListRootRaw = 0;
    if (!readU64(in, baselineTxnId) || !readU64(in, snapshotTxnId) ||
        !readU64(in, masterRootRaw) || !readU64(in, freeListRootRaw)) {
        return Status::Corruption("truncated incremental dump header");
    }

    if (!std::filesystem::exists(dbPath)) {
        return Status::InvalidArgument(
            "incremental load target must already exist at the exact prior baseline: " +
            dbPath.string());
    }

    api::EnvironmentOptions envOpts;
    if (options.envOptions) envOpts = *options.envOptions;
    envOpts.pageSize = pageSize;
    auto envR = api::Environment::open(dbPath, envOpts);
    if (!envR) return envR.status();
    api::Environment& env = envR.value();
    core::PageManager& pm = env.pageManager();

    if (pm.pageSize() != pageSize) {
        return Status::InvalidArgument("incremental dump pageSize does not match target database");
    }
    if (static_cast<std::uint64_t>(pm.lastCommittedTxnId()) != baselineTxnId) {
        return Status::InvalidArgument(
            "incremental dump baseline does not match target database's current txnId -- "
            "increments must be applied in order over their exact prior baseline");
    }

    IncrementalLoadReport report;
    while (true) {
        std::uint64_t pageIdRaw = 0;
        if (!readU64(in, pageIdRaw)) return Status::Corruption("truncated incremental dump");
        if (pageIdRaw == static_cast<std::uint64_t>(kInvalidPageId)) break;  // end sentinel
        PageId pageId = static_cast<PageId>(pageIdRaw);
        // `type` is a placeholder -- the immediately-following readExact
        // overwrites the whole page image (header included) verbatim.
        auto pageR = pm.writePageAt(pageId, static_cast<TxnId>(snapshotTxnId), core::PageType::Leaf);
        if (!pageR) return pageR.status();
        core::PageHeader* header = pageR.value();
        if (!readExact(in, header, pageSize)) {
            return Status::Corruption("truncated incremental dump page image");
        }
        // finalizePage re-derives header->payloadSize/itemCount from what
        // readExact just copied in (a no-op reassignment) and recomputes the
        // CRC (a no-op given identical bytes) -- but critically, when the
        // *destination* has encryption and/or the SHA-256 checksum bolt-on
        // enabled, this is also what actually (re-)encrypts this page into
        // the destination's mmap and writes its sidecar entries. Without
        // this call, an encrypted destination's plaintext would only ever
        // land in PageManager's in-process cache, never in the file itself.
        // No WAL is attached in this context, so finalizePage's WAL-logging
        // branch is a no-op here regardless.
        pm.finalizePage(header, header->payloadSize, header->itemCount);
        ++report.pagesLoaded;
        if (options.onProgress) options.onProgress(report.pagesLoaded);
    }

    Status ic = pm.installRecoveredCommit(static_cast<TxnId>(snapshotTxnId),
                                          static_cast<PageId>(masterRootRaw),
                                          static_cast<PageId>(freeListRootRaw),
                                          /*walSequence=*/0, core::SyncMode::Durable);
    if (!ic.ok()) return ic;
    return Result<IncrementalLoadReport>(report);
}

Result<CompactCopyReport> compactCopy(api::Environment& source, const std::filesystem::path& destPath,
                                      bool forceOverwrite, const CompactCopyOptions& options) {
    std::filesystem::path tmpDump = destPath;
    tmpDump += ".compact_tmp";
    std::error_code ec;
    std::filesystem::remove(tmpDump, ec);

    DumpOptions dumpOpts;
    auto dumpR = dumpToFile(source, tmpDump, dumpOpts);
    if (!dumpR) {
        std::filesystem::remove(tmpDump, ec);
        return dumpR.status();
    }

    LoadOptions loadOpts;
    loadOpts.forceOverwrite = forceOverwrite;
    loadOpts.onProgress = options.onProgress;
    auto loadR = loadFromFile(tmpDump, destPath, loadOpts);
    std::filesystem::remove(tmpDump, ec);  // best-effort cleanup either way
    if (!loadR) return loadR.status();

    CompactCopyReport report;
    report.entries = loadR.value().entries;
    return Result<CompactCopyReport>(report);
}

Result<RepairReport> repair(const std::filesystem::path& source,
                            const std::filesystem::path& dest,
                            const RepairOptions& options) {
    if (std::filesystem::exists(dest) && !options.forceOverwrite) {
        return Status::InvalidArgument("repair destination exists; pass forceOverwrite");
    }
    if (std::filesystem::exists(dest) && options.forceOverwrite) {
        std::error_code ec;
        std::filesystem::remove(dest, ec);
        std::filesystem::remove(dest.string() + ".lock", ec);
    }

    api::EnvironmentOptions srcOpts;
    srcOpts.readOnly = true;
    auto srcR = api::Environment::open(source, srcOpts);
    if (!srcR) return srcR.status();
    api::Environment& src = srcR.value();

    RepairReport report;
    auto vr = verifyEnv(src);
    if (vr) report.sourceVerify = vr.moveValue();
    else report.notes.push_back("pre-repair verify failed: " + vr.status().message());

    api::EnvironmentOptions dstOpts;
    if (options.copyLayout) {
        dstOpts.pageSize = src.options().pageSize;
        dstOpts.maxDatabaseSize = src.options().maxDatabaseSize;
    }
    auto dstR = api::Environment::open(dest, dstOpts);
    if (!dstR) return dstR.status();
    api::Environment& dst = dstR.value();

    auto txnR = src.beginRead();
    if (!txnR) {
        // Fall back: try to open a cursor via a best-effort empty salvage.
        report.notes.push_back("could not begin read on source: " + txnR.status().message());
        return Result<RepairReport>(std::move(report));
    }
    txn::ReadTransaction& rtxn = txnR.value();
    btree::Cursor cur = rtxn.openCursor();
    auto first = cur.first();
    if (!first) {
        report.notes.push_back("cursor.first failed: " + first.status().message());
        return Result<RepairReport>(std::move(report));
    }

    auto wtxnR = dst.beginWrite();
    if (!wtxnR) return wtxnR.status();
    txn::WriteTransaction wtxn = wtxnR.moveValue();
    constexpr std::uint64_t kBatch = 256;
    std::uint64_t consecutiveFail = 0;

    auto advance = [&]() -> Result<bool> {
        auto n = cur.next();
        if (!n) return n.status();
        return Result<bool>(n.value());
    };

    bool positioned = first.value();
    while (positioned) {
        auto kR = cur.key();
        auto vR = cur.value();
        if (!kR || !vR) {
            ++report.skipped;
            ++consecutiveFail;
            if (options.maxConsecutiveFailures &&
                consecutiveFail >= options.maxConsecutiveFailures) {
                report.notes.push_back("stopped after consecutive failures");
                break;
            }
            auto n = advance();
            if (!n) {
                report.notes.push_back("cursor advance failed: " + n.status().message());
                break;
            }
            positioned = n.value();
            continue;
        }
        consecutiveFail = 0;
        Status ps = wtxn.put(kR.value(), toByteView(vR.value()), true);
        if (!ps.ok()) {
            ++report.skipped;
            report.notes.push_back("put failed: " + ps.message());
        } else {
            ++report.salvaged;
        }
        if (options.onProgress) options.onProgress(report.salvaged, report.skipped);
        if ((report.salvaged + report.skipped) % kBatch == 0) {
            Status cs = wtxn.commit();
            if (!cs.ok()) return cs;
            auto next = dst.beginWrite();
            if (!next) return next.status();
            wtxn = next.moveValue();
        }
        auto n = advance();
        if (!n) {
            report.notes.push_back("cursor advance failed: " + n.status().message());
            break;
        }
        positioned = n.value();
    }

    Status cs = wtxn.commit();
    if (!cs.ok()) return cs;
    return Result<RepairReport>(std::move(report));
}

}  // namespace annpocketdb::backup
