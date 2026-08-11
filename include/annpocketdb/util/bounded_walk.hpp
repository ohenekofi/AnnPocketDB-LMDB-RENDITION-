#pragma once
//
// AnnPocketDb :: util/bounded_walk.hpp
//
// Caps chain and tree traversals that follow on-disk pointers so a corrupted
// overflowNext / child pointer cannot hang open() or overflow the stack.
//
#include <cstdint>
#include <string>

#include "annpocketdb/util/status.hpp"

namespace annpocketdb {

// No legitimate B+tree in this engine is deeper than this: with 4096-byte
// pages and ~16-byte keys, fanout is large enough that depth 64 would hold
// more entries than a 1 TiB database can store.
inline constexpr std::uint32_t kMaxTreeDepth = 64;

// Overflow / free-list chains cannot exceed the number of pages in the file;
// callers may pass pageCount() when known. This is a hard fallback when the
// caller does not have a page count (still finite, still terminates).
inline constexpr std::uint64_t kMaxChainStepsFallback = 1ull << 20;  // 1M pages

// Tiny step counter for while/for loops that follow on-disk links.
class BoundedWalk {
public:
    explicit BoundedWalk(std::uint64_t limit) : limit_(limit == 0 ? kMaxChainStepsFallback : limit) {}

    // Call once per hop. Returns Corruption when the bound is exceeded.
    Status step(const char* what = "chain") {
        if (++steps_ > limit_) {
            return Status::Corruption(std::string(what) +
                                      " traversal exceeded bound (" +
                                      std::to_string(limit_) + ")");
        }
        return Status::OK();
    }

    std::uint64_t steps() const { return steps_; }
    std::uint64_t limit() const { return limit_; }

private:
    std::uint64_t steps_ = 0;
    std::uint64_t limit_;
};

}  // namespace annpocketdb
