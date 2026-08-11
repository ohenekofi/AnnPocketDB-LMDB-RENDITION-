#include "annpocketdb/compression/lz4_codec.hpp"

#include <climits>
#include <cstring>

#include "lz4.h"

namespace annpocketdb::compression {

bool compress(ByteView input, ByteBuffer& out) {
    if (input.empty() || input.size() > static_cast<std::size_t>(INT_MAX)) return false;
    int srcSize = static_cast<int>(input.size());
    int bound = LZ4_compressBound(srcSize);
    if (bound <= 0) return false;

    ByteBuffer scratch(static_cast<std::size_t>(bound));
    int written = LZ4_compress_default(reinterpret_cast<const char*>(input.data()),
                                       reinterpret_cast<char*>(scratch.data()), srcSize, bound);
    if (written <= 0 || static_cast<std::size_t>(written) >= input.size()) {
        // Either the codec failed, or compression didn't actually shrink the
        // payload -- caller falls back to storing the original bytes.
        return false;
    }
    scratch.resize(static_cast<std::size_t>(written));
    out = std::move(scratch);
    return true;
}

Status decompress(ByteView input, std::uint64_t originalLen, ByteBuffer& out) {
    if (originalLen > static_cast<std::uint64_t>(INT_MAX) || input.size() > static_cast<std::size_t>(INT_MAX)) {
        return Status::Corruption("compressed value length out of supported range");
    }
    ByteBuffer buf(static_cast<std::size_t>(originalLen));
    int result = LZ4_decompress_safe(reinterpret_cast<const char*>(input.data()),
                                     reinterpret_cast<char*>(buf.data()),
                                     static_cast<int>(input.size()), static_cast<int>(originalLen));
    if (result < 0 || static_cast<std::uint64_t>(result) != originalLen) {
        return Status::Corruption("LZ4 decompression failed or produced an unexpected length");
    }
    out = std::move(buf);
    return Status::OK();
}

}  // namespace annpocketdb::compression
