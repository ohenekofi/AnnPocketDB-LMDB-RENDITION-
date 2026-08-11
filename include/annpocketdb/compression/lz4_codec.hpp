#pragma once
//
// AnnPocketDb :: compression/lz4_codec.hpp
//
// Value-level compression via vendored LZ4 (thirdparty/lz4/lz4.c, official
// "basic API", BSD 2-Clause). Only ever called from BTree on plain,
// non-dup, non-fixed-stride values (see BTree::maybeCompress) -- never on
// page bytes themselves (this engine's fixed-stride mmap page table can't
// benefit from page-level compression the way a variable-length block
// store could; see the compression Roadmap note in README.md).
//
// Built only under ANNPOCKETDB_WITH_COMPRESSION; this header is still safe
// to include unconditionally (declarations only cost nothing), but the
// corresponding .cpp is excluded from the kernel target when the macro is
// off, matching encryption/page_cipher.hpp's pattern.
//
#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

namespace annpocketdb::compression {

// Best-effort: compresses `input` into `out`. Returns false (leaving `out`
// unspecified) if compression would not shrink the payload or the codec
// otherwise fails -- callers must fall back to storing the original bytes
// uncompressed in that case. Never called on inputs larger than INT_MAX
// bytes (LZ4's API is int-sized); callers must guard that separately.
bool compress(ByteView input, ByteBuffer& out);

// Decompresses `input` into `out`, expecting exactly `originalLen` bytes of
// output. Status::Corruption if the codec fails or the decompressed length
// does not match `originalLen`.
Status decompress(ByteView input, std::uint64_t originalLen, ByteBuffer& out);

}  // namespace annpocketdb::compression
