// util::PageIdList: this project's midl.c equivalent (see
// util/page_id_list.hpp). Exercises both the order-preserving stack side
// (pushBack/popBack/appendRange -- what core::FreeList's reusable pool
// actually uses) and the sorted-array side (insertSorted/contains/
// eraseSorted/mergeSorted).
#include "test_common.hpp"

#include <vector>

#include "annpocketdb/util/page_id_list.hpp"

using namespace annpocketdb;
using namespace annpocketdb::util;

int main() {
    // ---- Stack discipline (LIFO, order-preserving) ----
    {
        PageIdList list;
        CHECK(list.empty());
        list.pushBack(5);
        list.pushBack(3);
        list.pushBack(9);
        CHECK(list.size() == 3);
        CHECK(list.popBack() == 9);
        CHECK(list.popBack() == 3);
        CHECK(list.popBack() == 5);
        CHECK(list.empty());
    }

    // ---- appendRange preserves the source's order (no implicit sort) ----
    {
        PageIdList list;
        list.pushBack(100);
        std::vector<PageId> more{7, 42, 7, 1};
        list.appendRange(more);
        CHECK(list.size() == 5);
        CHECK(list.raw()[0] == 100);
        CHECK(list.raw()[1] == 7);
        CHECK(list.raw()[2] == 42);
        CHECK(list.raw()[3] == 7);
        CHECK(list.raw()[4] == 1);
        CHECK(!list.isSorted());
    }

    // ---- releaseAll moves out and resets to empty ----
    {
        PageIdList list;
        list.pushBack(1);
        list.pushBack(2);
        auto out = list.releaseAll();
        CHECK(out.size() == 2 && out[0] == 1 && out[1] == 2);
        CHECK(list.empty());
    }

    // ---- Sorted-array discipline ----
    {
        PageIdList list;
        for (PageId id : {30, 10, 50, 20, 40}) list.insertSorted(id);
        CHECK(list.isSorted());
        CHECK(list.size() == 5);
        std::vector<PageId> expected{10, 20, 30, 40, 50};
        CHECK(list.raw() == expected);

        CHECK(list.contains(30));
        CHECK(!list.contains(35));

        CHECK(list.eraseSorted(30));
        CHECK(!list.contains(30));
        CHECK(list.size() == 4);
        CHECK(!list.eraseSorted(999));  // not present -- no-op, returns false
        CHECK(list.size() == 4);
        CHECK(list.isSorted());
    }

    // ---- mergeSorted combines two sorted lists, keeping duplicates ----
    {
        PageIdList a, b;
        for (PageId id : {1, 3, 5, 5}) a.insertSorted(id);
        for (PageId id : {2, 4, 5, 6}) b.insertSorted(id);
        a.mergeSorted(b);
        CHECK(a.isSorted());
        std::vector<PageId> expected{1, 2, 3, 4, 5, 5, 5, 6};
        CHECK(a.raw() == expected);
    }

    // ---- explicit sort() on unsorted content ----
    {
        PageIdList list;
        list.appendRange(std::vector<PageId>{9, 1, 5, 3});
        CHECK(!list.isSorted());
        list.sort();
        CHECK(list.isSorted());
        std::vector<PageId> expected{1, 3, 5, 9};
        CHECK(list.raw() == expected);
        CHECK(list.contains(5));
    }

    TEST_MAIN_END();
}
