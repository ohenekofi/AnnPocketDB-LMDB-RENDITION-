// platform::MappedFile smoke test: write -> flush -> grow well past the
// initial file size and confirm the base pointer never moves (pure
// ftruncate, no remap -- see mapped_file.hpp's growth-safety contract);
// confirms growth past the reserved ceiling fails cleanly with MapFull
// rather than corrupting the mapping or moving the base pointer; confirms
// persistence of written+flushed content across close/reopen, including a
// read-only reopen.
#include "test_common.hpp"

#include <cstring>
#include <string>

#include "annpocketdb/platform/mapped_file.hpp"

using namespace annpocketdb;
using namespace annpocketdb::platform;

int main() {
    auto path = anntest::scratchDbPath("mapped_file_raw.bin");

    constexpr std::uint64_t kPageSize = 4096;
    constexpr std::uint64_t kInitialSize = kPageSize * 4;    // 16 KiB
    constexpr std::uint64_t kMaxSize = kPageSize * 100;      // ~400 KiB reservation ceiling

    const std::string marker1 = "MAPPEDFILE_MARKER_start_0xCAFEBABE";
    const std::string marker2 = "MAPPEDFILE_MARKER_far_region_0xDEADBEEF";
    constexpr std::uint64_t kMarker2Offset = kPageSize * 40;

    void* originalBase = nullptr;

    // ---- Write, flush, grow-without-remap, and reject-past-ceiling ----
    {
        auto mfR = MappedFile::open(path, OpenMode::ReadWrite, kInitialSize, kMaxSize);
        CHECK(mfR.ok());
        if (!mfR) TEST_MAIN_END();
        MappedFile mf = mfR.moveValue();

        CHECK(mf.isOpen());
        CHECK(mf.fileSize() >= kInitialSize);
        CHECK(mf.mappedSize() >= kMaxSize);

        std::memcpy(mf.data(), marker1.data(), marker1.size());
        CHECK_OK(mf.flush(0, kPageSize, /*sync=*/true));

        originalBase = mf.data();

        // Grow well past the initial file size, but still within the
        // reserved ceiling: this is a pure ftruncate, so the base pointer
        // must not move -- a reader holding a raw pointer into the mapping
        // must never be invalidated by another thread's concurrent growth.
        std::uint64_t grownSize = kPageSize * 50;
        CHECK_OK(mf.resize(grownSize));
        CHECK(mf.fileSize() >= grownSize);
        CHECK(mf.data() == originalBase);

        // Write a second marker far into the newly-grown region and flush it.
        std::memcpy(mf.data() + kMarker2Offset, marker2.data(), marker2.size());
        CHECK_OK(mf.flush(kMarker2Offset, kPageSize, /*sync=*/true));

        // The original marker at offset 0 must still be intact -- growth
        // must not disturb already-written content.
        CHECK(std::memcmp(mf.data(), marker1.data(), marker1.size()) == 0);

        // Growing past the reserved ceiling must fail cleanly with MapFull,
        // not corrupt the mapping, not move the base pointer, and not
        // silently truncate to some in-between size. Test against the
        // *actual* reservation (mf.mappedSize()), not kMaxSize itself --
        // open() rounds the requested ceiling up to an internal allocation
        // granularity, so the real reservation can be larger than requested.
        std::uint64_t beforeFailSize = mf.fileSize();
        Status tooBig = mf.resize(mf.mappedSize() + kPageSize);
        CHECK(!tooBig.ok());
        CHECK(tooBig.code() == StatusCode::MapFull);
        CHECK(mf.data() == originalBase);
        CHECK(mf.fileSize() == beforeFailSize);

        // Content must still be intact after the rejected resize.
        CHECK(std::memcmp(mf.data(), marker1.data(), marker1.size()) == 0);
        CHECK(std::memcmp(mf.data() + kMarker2Offset, marker2.data(), marker2.size()) == 0);

        CHECK_OK(mf.close());
        CHECK(!mf.isOpen());
    }

    // ---- Persistence across close/reopen (read-write) ----
    {
        auto mfR = MappedFile::open(path, OpenMode::ReadWrite, /*initialSize=*/0, kMaxSize);
        CHECK(mfR.ok());
        if (mfR.ok()) {
            MappedFile mf = mfR.moveValue();
            CHECK(mf.fileSize() >= kPageSize * 50);
            CHECK(std::memcmp(mf.data(), marker1.data(), marker1.size()) == 0);
            CHECK(std::memcmp(mf.data() + kMarker2Offset, marker2.data(), marker2.size()) == 0);
            CHECK_OK(mf.close());
        }
    }

    // ---- Persistence across close/reopen (read-only) ----
    {
        auto mfR = MappedFile::open(path, OpenMode::ReadOnly, /*initialSize=*/0, kMaxSize);
        CHECK(mfR.ok());
        if (mfR.ok()) {
            MappedFile mf = mfR.moveValue();
            CHECK(std::memcmp(mf.data(), marker1.data(), marker1.size()) == 0);
            CHECK(std::memcmp(mf.data() + kMarker2Offset, marker2.data(), marker2.size()) == 0);
            CHECK_OK(mf.close());
        }
    }

    TEST_MAIN_END();
}