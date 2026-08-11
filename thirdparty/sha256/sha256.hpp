// sha256.hpp - Portable, single-header, dependency-free SHA-256 (FIPS 180-4)
//
// Public domain / CC0. No external deps, no build system, C++11.
// Produces standard-conformant SHA-256 digests (verified against NIST test
// vectors) — safe to use as fixed-size binary keys with LMDB, since LMDB's
// default comparator (mdb_cmp) does a raw memcmp on key bytes and a 32-byte
// digest sorts/compares correctly with no custom MDB_cmp_func needed.
//
// Usage:
//   #include "sha256.hpp"
//   auto digest = sha256::hash(data, len);      // std::array<uint8_t,32>
//   std::string hex = sha256::to_hex(digest);   // lowercase hex string
//
//   // Streaming:
//   sha256::Context ctx;
//   ctx.update(chunk1, len1);
//   ctx.update(chunk2, len2);
//   auto digest = ctx.finalize();

#pragma once
#include <cstdint>
#include <cstring>
#include <array>
#include <string>
#include <cstddef>

namespace sha256 {

namespace detail {

inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

// FIPS 180-4 §4.2.2 constants
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

} // namespace detail

class Context {
public:
    Context() { reset(); }

    void reset() {
        h_[0]=0x6a09e667; h_[1]=0xbb67ae85; h_[2]=0x3c6ef372; h_[3]=0xa54ff53a;
        h_[4]=0x510e527f; h_[5]=0x9b05688c; h_[6]=0x1f83d9ab; h_[7]=0x5be0cd19;
        bitlen_ = 0;
        buflen_ = 0;
    }

    void update(const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        bitlen_ += static_cast<uint64_t>(len) * 8;

        // Fill any partial buffer first
        if (buflen_ > 0) {
            size_t take = 64 - buflen_;
            if (take > len) take = len;
            std::memcpy(buf_ + buflen_, p, take);
            buflen_ += take;
            p += take;
            len -= take;
            if (buflen_ == 64) {
                transform(buf_);
                buflen_ = 0;
            }
        }

        // Process full 64-byte blocks directly
        while (len >= 64) {
            transform(p);
            p += 64;
            len -= 64;
        }

        // Stash remainder
        if (len > 0) {
            std::memcpy(buf_, p, len);
            buflen_ = len;
        }
    }

    std::array<uint8_t, 32> finalize() {
        uint8_t pad[72];
        size_t padlen = 0;
        pad[padlen++] = 0x80;

        size_t used = (buflen_ + 1) % 64;
        size_t zeros = (used <= 56) ? (56 - used) : (120 - used);
        std::memset(pad + padlen, 0, zeros);
        padlen += zeros;

        uint64_t bl = bitlen_;
        for (int i = 7; i >= 0; --i) {
            pad[padlen++] = static_cast<uint8_t>(bl >> (i * 8));
        }

        update(pad, padlen); // note: this changes bitlen_, but we've already snapshot it above

        std::array<uint8_t, 32> out;
        for (int i = 0; i < 8; ++i) {
            out[i*4 + 0] = static_cast<uint8_t>(h_[i] >> 24);
            out[i*4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
            out[i*4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
            out[i*4 + 3] = static_cast<uint8_t>(h_[i]);
        }
        return out;
    }

private:
    void transform(const uint8_t block[64]) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i*4]) << 24) |
                   (static_cast<uint32_t>(block[i*4+1]) << 16) |
                   (static_cast<uint32_t>(block[i*4+2]) << 8) |
                   (static_cast<uint32_t>(block[i*4+3]));
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = detail::rotr(w[i-15], 7) ^ detail::rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = detail::rotr(w[i-2], 17) ^ detail::rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }

        uint32_t a=h_[0], b=h_[1], c=h_[2], d=h_[3], e=h_[4], f=h_[5], g=h_[6], h=h_[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = detail::rotr(e,6) ^ detail::rotr(e,11) ^ detail::rotr(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t temp1 = h + S1 + ch + detail::K[i] + w[i];
            uint32_t S0 = detail::rotr(a,2) ^ detail::rotr(a,13) ^ detail::rotr(a,22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;

            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        h_[0]+=a; h_[1]+=b; h_[2]+=c; h_[3]+=d;
        h_[4]+=e; h_[5]+=f; h_[6]+=g; h_[7]+=h;
    }

    uint32_t h_[8];
    uint64_t bitlen_;
    uint8_t buf_[64];
    size_t buflen_;
};

// One-shot convenience API
inline std::array<uint8_t, 32> hash(const void* data, size_t len) {
    Context ctx;
    ctx.update(data, len);
    return ctx.finalize();
}

inline std::array<uint8_t, 32> hash(const std::string& s) {
    return hash(s.data(), s.size());
}

inline std::string to_hex(const std::array<uint8_t, 32>& digest) {
    static const char* lut = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint8_t b : digest) {
        out.push_back(lut[b >> 4]);
        out.push_back(lut[b & 0x0F]);
    }
    return out;
}

} // namespace sha256
