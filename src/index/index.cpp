#include "annpocketdb/index/index.hpp"

#include <algorithm>
#include <cstring>

#include "annpocketdb/btree/node_codec.hpp"

namespace annpocketdb::index {

// ---------------------------------------------------------------------------
// Index (reads)
// ---------------------------------------------------------------------------

Result<btree::Cursor> Index::find(ByteView secondaryKeyPrefix) const {
    if (rootPageId_ == kInvalidPageId) return Status::NotFound("index is empty");
    btree::Cursor cursor(tree_, [this]() { return rootPageId_; });
    auto r = cursor.seek(secondaryKeyPrefix);
    if (!r) return r.status();
    if (!r.value()) return Status::NotFound("no entries for secondary key");
    auto k = cursor.key();
    if (!k) return k.status();
    // DUPSORT layout: leaf key == secondary, leaf value == primary.
    // Prefix match on the secondary key.
    ByteView sec = k.value();
    if (sec.size() < secondaryKeyPrefix.size() ||
        compareBytes(ByteView(sec.data(), secondaryKeyPrefix.size()), secondaryKeyPrefix) != 0) {
        return Status::NotFound("no entries for secondary key");
    }
    return Result<btree::Cursor>(std::move(cursor));
}


Result<btree::Cursor> Index::seekLowerBound(ByteView lower) const {
    if (rootPageId_ == kInvalidPageId) return Status::NotFound("index is empty");
    btree::Cursor cursor(tree_, [this]() { return rootPageId_; });
    if (lower.empty()) {
        auto r = cursor.first();
        if (!r) return r.status();
        if (!r.value()) return Status::NotFound("index is empty");
        return Result<btree::Cursor>(std::move(cursor));
    }
    auto r = cursor.seek(lower);
    if (!r) return r.status();
    if (!r.value()) return Status::NotFound("no entries >= lower bound");
    return Result<btree::Cursor>(std::move(cursor));
}

Result<btree::Cursor> Index::openCursor() const {
    return seekLowerBound(ByteView{});
}

Result<std::uint64_t> Index::entryCount() const {
    if (rootPageId_ == kInvalidPageId) return Result<std::uint64_t>(0);
    btree::Cursor cursor(tree_, [this]() { return rootPageId_; });
    auto r = cursor.first();
    if (!r) return r.status();
    std::uint64_t count = 0;
    while (cursor.valid()) {
        ++count;
        auto n = cursor.next();
        if (!n) return n.status();
        if (!n.value()) break;
    }
    return Result<std::uint64_t>(count);
}

// ---------------------------------------------------------------------------
// IndexManager
// ---------------------------------------------------------------------------

IndexManager::IndexManager(btree::BTree& mainTree, core::PageManager& pm)
    : mainTree_(mainTree), pm_(pm) {}

Status IndexManager::ensureLoaded() const {
    if (!catalogLoaded_) return Status::Internal("index catalog not loaded");
    return Status::OK();
}

Status IndexManager::loadCatalog(txn::ReadTransaction& txn) {
    auto r = txn.get(toByteView(kCatalogKey));
    if (!r) {
        if (r.status().code() == StatusCode::NotFound) {
            catalog_.clear();
            catalogLoaded_ = true;
            return Status::OK();
        }
        return r.status();
    }
    return decodeCatalog(ByteView(r.value().data(), r.value().size()));
}

Status IndexManager::saveCatalog(txn::WriteTransaction& txn) {
    Status el = ensureLoaded();
    if (!el.ok()) return el;
    auto enc = encodeCatalog();
    if (!enc) return enc.status();
    // Master-keyspace put so commit() publishes the catalog with the meta root.
    return txn.put(toByteView(kCatalogKey), ByteView(enc.value().data(), enc.value().size()), true);
}

Status IndexManager::createIndex(const std::string& name, const std::string& fieldPath,
                                  txn::WriteTransaction& txn) {
    Status el = ensureLoaded();
    if (!el.ok()) return el;
    if (name.empty()) return Status::InvalidArgument("index name empty");
    if (fieldPath.empty()) return Status::InvalidArgument("index fieldPath empty");
    if (catalog_.find(name) != catalog_.end()) return Status::KeyExists("index already exists");

    auto alloc = pm_.allocatePage(txn.txnId(), core::PageType::Leaf);
    if (!alloc) return alloc.status();
    core::PageHeader* header = alloc.value();
    pm_.finalizePage(header, 0, 0);

    IndexMeta meta;
    meta.rootPageId = header->pageId;
    meta.fieldPath = fieldPath;
    catalog_[name] = std::move(meta);
    (void)txn;
    return Status::OK();
}

Status IndexManager::collectReachablePages(core::PageManager& pm, PageId root,
                                            std::vector<PageId>& out) {
    out.clear();
    if (root == kInvalidPageId) return Status::OK();

    std::vector<PageId> stack;
    stack.push_back(root);
    // Dedup in case of unexpected cycles (should not happen in a well-formed tree).
    std::unordered_map<PageId, bool> seen;

    while (!stack.empty()) {
        PageId pid = stack.back();
        stack.pop_back();
        if (pid == kInvalidPageId) continue;
        if (seen[pid]) continue;
        seen[pid] = true;
        out.push_back(pid);

        auto pageResult = pm.getPage(pid);
        if (!pageResult) {
            // Page unreadable — still listed for retire; stop descending.
            continue;
        }
        const core::PageHeader* h = pageResult.value();
        if (h->type == core::PageType::Branch) {
            btree::BranchPageView v(h);
            for (std::uint16_t j = 0; j < v.count(); ++j) {
                stack.push_back(v.childAt(j));
            }
        } else if (h->type == core::PageType::Leaf) {
            btree::LeafPageView v(h);
            for (std::uint16_t j = 0; j < v.count(); ++j) {
                if (v.isOverflowAt(j)) {
                    PageId ov = v.overflowFirstPageAt(j);
                    while (ov != kInvalidPageId && !seen[ov]) {
                        seen[ov] = true;
                        out.push_back(ov);
                        auto ovPage = pm.getPage(ov);
                        if (!ovPage) break;
                        ov = ovPage.value()->overflowNext;
                    }
                }
            }
        } else if (h->type == core::PageType::Overflow) {
            PageId next = h->overflowNext;
            if (next != kInvalidPageId) stack.push_back(next);
        }
    }
    return Status::OK();
}

Status IndexManager::dropIndex(const std::string& name, txn::WriteTransaction& txn) {
    Status el = ensureLoaded();
    if (!el.ok()) return el;
    auto it = catalog_.find(name);
    if (it == catalog_.end()) return Status::NotFound("index not found");

    std::vector<PageId> pages;
    Status cs = collectReachablePages(pm_, it->second.rootPageId, pages);
    if (!cs.ok()) return cs;
    for (PageId pid : pages) {
        pm_.retirePage(pid, txn.txnId());
    }
    catalog_.erase(it);
    return Status::OK();
}

Result<Index> IndexManager::openIndex(const std::string& name) const {
    Status el = ensureLoaded();
    if (!el.ok()) return el;
    auto it = catalog_.find(name);
    if (it == catalog_.end()) return Status::NotFound("index not found");
    return Result<Index>(Index(mainTree_, it->second.rootPageId, name, it->second.fieldPath));
}

bool IndexManager::hasIndex(const std::string& name) const {
    return catalog_.find(name) != catalog_.end();
}

std::vector<std::string> IndexManager::listIndexes() const {
    std::vector<std::string> names;
    names.reserve(catalog_.size());
    for (const auto& [name, meta] : catalog_) {
        (void)meta;
        names.push_back(name);
    }
    return names;
}

Result<IndexMeta> IndexManager::meta(const std::string& name) const {
    Status el = ensureLoaded();
    if (!el.ok()) return el;
    auto it = catalog_.find(name);
    if (it == catalog_.end()) return Status::NotFound("index not found");
    return Result<IndexMeta>(it->second);
}

Status IndexManager::insertEntry(txn::WriteTransaction& txn, const std::string& name,
                                  ByteView secondaryKey, ByteView primaryKey) {
    Status el = ensureLoaded();
    if (!el.ok()) return el;
    auto it = catalog_.find(name);
    if (it == catalog_.end()) return Status::NotFound("index not found");

    PageId root = it->second.rootPageId;
    // DUPSORT: secondary key → primary key as value (multiple primaries per secondary).
    Status s = txn.putDup(root, secondaryKey, primaryKey, /*noDupData=*/true);
    if (!s.ok()) {
        // Idempotent: exact pair already present.
        if (s.code() == StatusCode::KeyExists) return Status::OK();
        return s;
    }
    it->second.rootPageId = root;
    return Status::OK();
}

Status IndexManager::removeEntry(txn::WriteTransaction& txn, const std::string& name,
                                  ByteView secondaryKey, ByteView primaryKey) {
    Status el = ensureLoaded();
    if (!el.ok()) return el;
    auto it = catalog_.find(name);
    if (it == catalog_.end()) return Status::NotFound("index not found");

    PageId root = it->second.rootPageId;
    Status s = txn.removeDup(root, secondaryKey, primaryKey);
    if (!s.ok()) {
        if (s.code() == StatusCode::NotFound) return Status::OK();
        return s;
    }
    it->second.rootPageId = root;
    return Status::OK();
}

namespace {

Status extractSecKeys(document::IDocumentCodec& codec, ByteView doc,
                      const std::string& fieldPath, std::vector<ByteBuffer>& out) {
    out.clear();
    if (doc.empty()) return Status::OK();
    document::FieldPath path = document::parseFieldPath(fieldPath);
    Status s = document::extractIndexKeys(codec, doc, path, out);
    if (!s.ok()) {
        if (s.code() == StatusCode::NotFound) return Status::OK();
        return s;
    }
    return Status::OK();
}

bool sameKeySet(std::vector<ByteBuffer>& a, std::vector<ByteBuffer>& b) {
    if (a.size() != b.size()) return false;
    auto norm = [](std::vector<ByteBuffer>& v) {
        std::sort(v.begin(), v.end(), [](const ByteBuffer& x, const ByteBuffer& y) {
            return compareBytes(ByteView(x.data(), x.size()), ByteView(y.data(), y.size())) < 0;
        });
    };
    norm(a);
    norm(b);
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].size() != b[i].size()) return false;
        if (a[i].size() && std::memcmp(a[i].data(), b[i].data(), a[i].size()) != 0) return false;
    }
    return true;
}

}  // namespace

Status IndexManager::maintainOnPut(txn::WriteTransaction& txn,
                                    ByteView primaryKey,
                                    ByteView oldDoc,
                                    ByteView newDoc,
                                    document::IDocumentCodec& codec) {
    Status el = ensureLoaded();
    if (!el.ok()) return el;

    for (auto& [name, meta] : catalog_) {
        std::vector<ByteBuffer> oldKeys, newKeys;
        Status s = extractSecKeys(codec, oldDoc, meta.fieldPath, oldKeys);
        if (!s.ok()) return s;
        s = extractSecKeys(codec, newDoc, meta.fieldPath, newKeys);
        if (!s.ok()) return s;

        if (sameKeySet(oldKeys, newKeys)) continue;

        for (const auto& k : oldKeys) {
            s = removeEntry(txn, name, ByteView(k.data(), k.size()), primaryKey);
            if (!s.ok()) return s;
        }
        for (const auto& k : newKeys) {
            s = insertEntry(txn, name, ByteView(k.data(), k.size()), primaryKey);
            if (!s.ok() && s.code() != StatusCode::KeyExists) return s;
        }
    }
    return Status::OK();
}

Status IndexManager::maintainOnRemove(txn::WriteTransaction& txn,
                                       ByteView primaryKey,
                                       ByteView oldDoc,
                                       document::IDocumentCodec& codec) {
    return maintainOnPut(txn, primaryKey, oldDoc, ByteView{}, codec);
}

// Catalog v2: [u32 version=2] { u32 nameLen; name; u32 pathLen; path; u64 root }*
// Catalog v1 (legacy): { u32 nameLen; name; u64 root }*  — path defaults to name

Status IndexManager::decodeCatalog(ByteView data) {
    catalog_.clear();
    if (data.empty()) {
        catalogLoaded_ = true;
        return Status::OK();
    }

    std::size_t off = 0;
    std::uint32_t version = 1;
    if (data.size() >= 4) {
        std::uint32_t maybe;
        std::memcpy(&maybe, data.data(), 4);
        if (maybe == kCatalogVersion) {
            version = maybe;
            off = 4;
        }
    }

    while (off < data.size()) {
        if (off + 4 > data.size()) return Status::Corruption("catalog name length truncated");
        std::uint32_t nameLen;
        std::memcpy(&nameLen, data.data() + off, sizeof(nameLen));
        off += 4;
        if (off + nameLen > data.size()) return Status::Corruption("catalog name truncated");
        std::string name(reinterpret_cast<const char*>(data.data() + off), nameLen);
        off += nameLen;

        IndexMeta meta;
        if (version >= 2) {
            if (off + 4 > data.size()) return Status::Corruption("catalog path length truncated");
            std::uint32_t pathLen;
            std::memcpy(&pathLen, data.data() + off, sizeof(pathLen));
            off += 4;
            if (off + pathLen > data.size()) return Status::Corruption("catalog path truncated");
            meta.fieldPath.assign(reinterpret_cast<const char*>(data.data() + off), pathLen);
            off += pathLen;
        } else {
            meta.fieldPath = name;  // legacy: index name was the field
        }

        if (off + 8 > data.size()) return Status::Corruption("catalog root truncated");
        std::uint64_t root;
        std::memcpy(&root, data.data() + off, sizeof(root));
        off += 8;
        meta.rootPageId = root;
        catalog_[std::move(name)] = std::move(meta);
    }
    catalogLoaded_ = true;
    return Status::OK();
}

Result<ByteBuffer> IndexManager::encodeCatalog() const {
    ByteBuffer out;
    // version header
    std::uint32_t ver = kCatalogVersion;
    out.resize(4);
    std::memcpy(out.data(), &ver, 4);

    for (const auto& [name, meta] : catalog_) {
        std::uint32_t nameLen = static_cast<std::uint32_t>(name.size());
        std::uint32_t pathLen = static_cast<std::uint32_t>(meta.fieldPath.size());
        std::size_t off = out.size();
        out.resize(off + 4 + nameLen + 4 + pathLen + 8);
        std::memcpy(out.data() + off, &nameLen, 4);
        std::memcpy(out.data() + off + 4, name.data(), nameLen);
        std::memcpy(out.data() + off + 4 + nameLen, &pathLen, 4);
        std::memcpy(out.data() + off + 4 + nameLen + 4, meta.fieldPath.data(), pathLen);
        std::uint64_t root = meta.rootPageId;
        std::memcpy(out.data() + off + 4 + nameLen + 4 + pathLen, &root, 8);
    }
    return Result<ByteBuffer>(std::move(out));
}

}  // namespace annpocketdb::index
