#pragma once
//
// AnnPocketDb :: index/index.hpp
//
// Secondary index manager. Each index is an auxiliary BTree keyed by
// composite entries: [secondary_key][0x00][primary_key]. Values are empty.
//
// Consistency rules (production):
//   - All index mutations go through WriteTransaction::put/remove(PageId&),
//     so COW root updates are explicit.
//   - IndexManager::catalog_ is updated immediately after each mutation and
//     persisted via saveCatalog() into the *master* keyspace under
//     __sys_idx_catalog__ before commit().
//   - createIndex stores a field path; maintainOnPut/Remove use that path
//     with an IDocumentCodec to keep indexes in sync with primary docs.
//   - dropIndex walks the full tree (branches, leaves, overflow chains) and
//     retires every reachable page under the writer txn id.
//
#include <string>
#include <unordered_map>
#include <vector>

#include "annpocketdb/btree/btree.hpp"
#include "annpocketdb/btree/cursor.hpp"
#include "annpocketdb/core/page_manager.hpp"
#include "annpocketdb/document/document.hpp"
#include "annpocketdb/txn/transaction.hpp"
#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

namespace annpocketdb::index {

inline ByteBuffer makeIndexKey(ByteView secondaryKey, ByteView primaryKey) {
    ByteBuffer key;
    key.reserve(secondaryKey.size() + 1 + primaryKey.size());
    key.insert(key.end(), secondaryKey.begin(), secondaryKey.end());
    key.push_back(std::byte{0});
    key.insert(key.end(), primaryKey.begin(), primaryKey.end());
    return key;
}

inline Status splitIndexKey(ByteView composite, ByteView& secondaryKey, ByteView& primaryKey) {
    for (std::size_t i = 0; i < composite.size(); ++i) {
        if (composite[i] == std::byte{0}) {
            secondaryKey = ByteView(composite.data(), i);
            primaryKey = ByteView(composite.data() + i + 1, composite.size() - i - 1);
            return Status::OK();
        }
    }
    return Status::Corruption("invalid composite index key: missing separator");
}

// ---------------------------------------------------------------------------
// Index — read-focused handle; writes go through IndexManager so the catalog
// root stays authoritative.
// ---------------------------------------------------------------------------

class Index {
public:
    Index(btree::BTree& tree, PageId rootPageId, std::string name, std::string fieldPath)
        : tree_(tree), rootPageId_(rootPageId), name_(std::move(name)),
          fieldPath_(std::move(fieldPath)) {}

    // Read API
    Result<btree::Cursor> find(ByteView secondaryKeyPrefix) const;

    // Cursor at first entry with secondary key >= lower (or first entry if lower empty).
    Result<btree::Cursor> seekLowerBound(ByteView lower) const;
    // Cursor at the first entry of the index (empty index → NotFound).
    Result<btree::Cursor> openCursor() const;
    Result<std::uint64_t> entryCount() const;

    PageId rootPageId() const { return rootPageId_; }
    const std::string& name() const { return name_; }
    const std::string& fieldPath() const { return fieldPath_; }

    void setRootPageId(PageId id) { rootPageId_ = id; }

private:
    friend class IndexManager;
    btree::BTree& tree_;
    PageId rootPageId_;
    std::string name_;
    std::string fieldPath_;
};

// ---------------------------------------------------------------------------
// IndexManager
// ---------------------------------------------------------------------------

struct IndexMeta {
    PageId rootPageId = kInvalidPageId;
    std::string fieldPath;  // dotted path, e.g. "user.email"
};

class IndexManager {
public:
    explicit IndexManager(btree::BTree& mainTree, core::PageManager& pm);

    Status loadCatalog(txn::ReadTransaction& txn);
    // Persist catalog into the master keyspace (must run before commit).
    Status saveCatalog(txn::WriteTransaction& txn);

    // Create empty index bound to fieldPath. Catalog updated in memory;
    // call saveCatalog before commit.
    Status createIndex(const std::string& name, const std::string& fieldPath,
                       txn::WriteTransaction& txn);

    // Full tree walk + retire; removes from catalog. Call saveCatalog after.
    Status dropIndex(const std::string& name, txn::WriteTransaction& txn);

    Result<Index> openIndex(const std::string& name) const;
    bool hasIndex(const std::string& name) const;
    std::vector<std::string> listIndexes() const;
    Result<IndexMeta> meta(const std::string& name) const;

    // ---- Mutators that keep catalog roots in sync ----
    // Use WriteTransaction multi-root put/remove; update catalog_[name].root.

    Status insertEntry(txn::WriteTransaction& txn, const std::string& name,
                       ByteView secondaryKey, ByteView primaryKey);
    Status removeEntry(txn::WriteTransaction& txn, const std::string& name,
                       ByteView secondaryKey, ByteView primaryKey);

    // ---- Automatic maintenance on primary document put/remove ----
    // For every registered index, extract fieldPath from old/new docs via
    // codec and insert/remove secondary keys as needed.
    //
    // oldDoc empty => pure insert; newDoc empty => pure delete.
    Status maintainOnPut(txn::WriteTransaction& txn,
                         ByteView primaryKey,
                         ByteView oldDoc,
                         ByteView newDoc,
                         document::IDocumentCodec& codec);
    Status maintainOnRemove(txn::WriteTransaction& txn,
                            ByteView primaryKey,
                            ByteView oldDoc,
                            document::IDocumentCodec& codec);

    // Collect every page id reachable from root (branches, leaves, overflow).
    static Status collectReachablePages(core::PageManager& pm, PageId root,
                                        std::vector<PageId>& out);

private:
    static constexpr std::string_view kCatalogKey = "__sys_idx_catalog__";
    static constexpr std::uint32_t kCatalogVersion = 2;

    Status decodeCatalog(ByteView data);
    Result<ByteBuffer> encodeCatalog() const;
    Status ensureLoaded() const;

    btree::BTree& mainTree_;
    core::PageManager& pm_;
    std::unordered_map<std::string, IndexMeta> catalog_;
    bool catalogLoaded_ = false;
};

}  // namespace annpocketdb::index
