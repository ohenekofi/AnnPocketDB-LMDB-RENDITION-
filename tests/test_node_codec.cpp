// btree::node_codec smoke test: encode/decode round trip for leaf and
// branch slotted-page nodes, including an overflow-marked leaf entry, plus
// binary-search (lowerBound / findChildIndex) correctness for exact,
// missing, and boundary keys. Operates directly on a heap-allocated,
// zeroed page buffer -- no PageManager/file involved, matching how
// encodeLeaf/encodeBranch are meant to be exercised in isolation from the
// rest of the storage stack.
#include "test_common.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "annpocketdb/btree/node_codec.hpp"
#include "annpocketdb/core/page.hpp"

using namespace annpocketdb;
using namespace annpocketdb::btree;
using namespace annpocketdb::core;

namespace {
// A fresh, zeroed page-sized buffer with pageId/type set on its header,
// matching the shape PageManager::allocatePage hands to the codec layer in
// the real write path.
std::vector<std::byte> makePage(std::uint32_t pageSize, PageType type, PageId id) {
    std::vector<std::byte> buf(pageSize, std::byte{0});
    PageHeader* h = reinterpret_cast<PageHeader*>(buf.data());
    h->pageId = id;
    h->type = type;
    return buf;
}
}  // namespace

int main() {
    constexpr std::uint32_t kPageSize = 4096;
    const std::uint32_t capacity = static_cast<std::uint32_t>(payloadCapacity(kPageSize));

    // ---- Leaf round trip: inline entries + one overflow-marked entry ----
    {
        std::vector<std::byte> buf = makePage(kPageSize, PageType::Leaf, 10);
        PageHeader* h = reinterpret_cast<PageHeader*>(buf.data());

        std::vector<LeafEntry> entries;
        auto addInline = [&](const std::string& k, const std::string& v) {
            LeafEntry e;
            e.key = toByteBuffer(k);
            e.value = toByteBuffer(v);
            entries.push_back(std::move(e));
        };
        addInline("apple", "fruit1");
        addInline("banana", "fruit2");
        addInline("cherry", "fruit3");

        // Overflow-marked entry; sorts between "cherry" and "mango".
        LeafEntry ov;
        ov.key = toByteBuffer(std::string("grape"));
        ov.isOverflow = true;
        ov.overflowFirstPage = 777;
        ov.overflowTotalLen = 123456;
        entries.push_back(ov);

        addInline("mango", "fruit4");

        CHECK(entries.size() == 5);
        CHECK(encodedLeafSize(entries) <= capacity);
        CHECK(encodeLeaf(h, capacity, entries));

        LeafPageView view(h);
        CHECK(view.count() == 5);

        // decodeAll must round-trip every field exactly, sorted:
        // apple(0), banana(1), cherry(2), grape(3, overflow), mango(4).
        auto decoded = view.decodeAll();
        CHECK(decoded.size() == 5);

        CHECK(toStringView(toByteView(decoded[0].key)) == "apple");
        CHECK(toStringView(toByteView(decoded[0].value)) == "fruit1");
        CHECK(!decoded[0].isOverflow);

        CHECK(toStringView(toByteView(decoded[1].key)) == "banana");
        CHECK(toStringView(toByteView(decoded[1].value)) == "fruit2");

        CHECK(toStringView(toByteView(decoded[2].key)) == "cherry");
        CHECK(toStringView(toByteView(decoded[2].value)) == "fruit3");

        CHECK(toStringView(toByteView(decoded[3].key)) == "grape");
        CHECK(decoded[3].isOverflow);
        CHECK(decoded[3].overflowFirstPage == 777);
        CHECK(decoded[3].overflowTotalLen == 123456);

        CHECK(toStringView(toByteView(decoded[4].key)) == "mango");
        CHECK(toStringView(toByteView(decoded[4].value)) == "fruit4");
        CHECK(!decoded[4].isOverflow);

        // View-level overflow accessors must agree with decodeAll.
        CHECK(!view.isOverflowAt(0));
        CHECK(!view.isOverflowAt(2));
        CHECK(view.isOverflowAt(3));
        CHECK(view.overflowFirstPageAt(3) == 777);
        CHECK(view.overflowTotalLenAt(3) == 123456);
        CHECK(!view.isOverflowAt(4));

        // keyAt must agree with decodeAll for every index.
        for (std::uint16_t i = 0; i < view.count(); ++i) {
            CHECK(toStringView(view.keyAt(i)) == toStringView(toByteView(decoded[i].key)));
        }

        // ---- Binary search (lowerBound) correctness ----
        // Exact match on every key.
        CHECK(view.lowerBound(toByteView(std::string("apple"))) == 0);
        CHECK(view.lowerBound(toByteView(std::string("banana"))) == 1);
        CHECK(view.lowerBound(toByteView(std::string("cherry"))) == 2);
        CHECK(view.lowerBound(toByteView(std::string("grape"))) == 3);
        CHECK(view.lowerBound(toByteView(std::string("mango"))) == 4);

        // Missing key strictly between two existing keys lands on the next
        // entry (standard lower_bound semantics).
        CHECK(view.lowerBound(toByteView(std::string("blueberry"))) == 2);  // banana < blueberry < cherry
        CHECK(view.lowerBound(toByteView(std::string("fig"))) == 3);        // cherry < fig < grape

        // Boundary: a key before everything lands at 0; a key after
        // everything lands at count().
        CHECK(view.lowerBound(toByteView(std::string("aardvark"))) == 0);
        CHECK(view.lowerBound(toByteView(std::string("zebra"))) == 5);
    }

    // ---- Leaf: empty entry set ----
    {
        std::vector<std::byte> buf = makePage(kPageSize, PageType::Leaf, 11);
        PageHeader* h = reinterpret_cast<PageHeader*>(buf.data());
        std::vector<LeafEntry> empty;
        CHECK(encodeLeaf(h, capacity, empty));
        LeafPageView view(h);
        CHECK(view.count() == 0);
        CHECK(view.lowerBound(toByteView(std::string("anything"))) == 0);
        CHECK(view.decodeAll().empty());
    }

    // ---- Leaf: encodeLeaf must fail (return false), not corrupt the
    // header, when entries do not fit in the given capacity ----
    {
        std::vector<std::byte> buf = makePage(kPageSize, PageType::Leaf, 12);
        PageHeader* h = reinterpret_cast<PageHeader*>(buf.data());
        std::vector<LeafEntry> huge(1);
        huge[0].key = toByteBuffer(std::string("k"));
        huge[0].value = ByteBuffer(capacity + 1, std::byte{'x'});
        CHECK(encodedLeafSize(huge) > capacity);
        CHECK(!encodeLeaf(h, capacity, huge));
    }

    // ---- Branch round trip ----
    {
        std::vector<std::byte> buf = makePage(kPageSize, PageType::Branch, 20);
        PageHeader* h = reinterpret_cast<PageHeader*>(buf.data());

        std::vector<BranchEntry> entries;
        // Entry 0's key is always empty ("-infinity"), per the format.
        entries.push_back(BranchEntry{ByteBuffer{}, 100});
        {
            BranchEntry e;
            e.key = toByteBuffer(std::string("m"));
            e.child = 200;
            entries.push_back(e);
        }
        {
            BranchEntry e;
            e.key = toByteBuffer(std::string("t"));
            e.child = 300;
            entries.push_back(e);
        }

        CHECK(encodedBranchSize(entries) <= capacity);
        CHECK(encodeBranch(h, capacity, entries));

        BranchPageView view(h);
        CHECK(view.count() == 3);
        auto decoded = view.decodeAll();
        CHECK(decoded.size() == 3);
        CHECK(decoded[0].key.empty());
        CHECK(decoded[0].child == 100);
        CHECK(toStringView(toByteView(decoded[1].key)) == "m");
        CHECK(decoded[1].child == 200);
        CHECK(toStringView(toByteView(decoded[2].key)) == "t");
        CHECK(decoded[2].child == 300);

        for (std::uint16_t i = 0; i < view.count(); ++i) {
            CHECK(view.childAt(i) == decoded[i].child);
        }

        // findChildIndex: largest i such that keyAt(i) <= key (entry 0's
        // empty key is always considered <= everything).
        CHECK(view.findChildIndex(toByteView(std::string("a"))) == 0);  // before "m" -> entry 0
        CHECK(view.findChildIndex(toByteView(std::string("m"))) == 1);  // exact "m"
        CHECK(view.findChildIndex(toByteView(std::string("q"))) == 1);  // between "m" and "t"
        CHECK(view.findChildIndex(toByteView(std::string("t"))) == 2);  // exact "t"
        CHECK(view.findChildIndex(toByteView(std::string("z"))) == 2);  // past everything -> last entry
    }

    // ---- Branch: single entry (entry 0 only, the minimal valid branch) ----
    {
        std::vector<std::byte> buf = makePage(kPageSize, PageType::Branch, 21);
        PageHeader* h = reinterpret_cast<PageHeader*>(buf.data());
        std::vector<BranchEntry> entries{BranchEntry{ByteBuffer{}, 42}};
        CHECK(encodeBranch(h, capacity, entries));
        BranchPageView view(h);
        CHECK(view.count() == 1);
        CHECK(view.findChildIndex(toByteView(std::string("anything"))) == 0);
        CHECK(view.childAt(0) == 42);
    }

    TEST_MAIN_END();
}