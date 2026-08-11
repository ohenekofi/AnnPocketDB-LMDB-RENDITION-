#include "annpocketdb/encryption/chacha8.hpp"

#include <cstring>

namespace annpocketdb::encryption {

namespace {

inline std::uint32_t rotl(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

void quarterRound(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c, std::uint32_t& d) {
    a += b; d ^= a; d = rotl(d, 16);
    c += d; b ^= c; b = rotl(b, 12);
    a += b; d ^= a; d = rotl(d, 8);
    c += d; b ^= c; b = rotl(b, 7);
}

// Little-endian word layout, matching the RFC 8439 ChaCha convention -- fine
// on this project's supported (little-endian) platforms; not a portability
// concern worth extra code for a primitive that is explicitly not for real
// security use.
void chacha8Block(const std::uint8_t key[kChaCha8KeySize], const std::uint8_t nonce[kChaCha8NonceSize],
                  std::uint32_t counter, std::uint8_t out[64]) {
    std::uint32_t state[16];
    state[0] = 0x61707865u; state[1] = 0x3320646eu; state[2] = 0x79622d32u; state[3] = 0x6b206574u;
    std::memcpy(&state[4], key, kChaCha8KeySize);
    state[12] = counter;
    std::memcpy(&state[13], nonce, kChaCha8NonceSize);

    std::uint32_t working[16];
    std::memcpy(working, state, sizeof(working));

    for (int round = 0; round < 4; ++round) {  // 4 double-rounds = 8 rounds (literal ChaCha8)
        quarterRound(working[0], working[4], working[8], working[12]);
        quarterRound(working[1], working[5], working[9], working[13]);
        quarterRound(working[2], working[6], working[10], working[14]);
        quarterRound(working[3], working[7], working[11], working[15]);
        quarterRound(working[0], working[5], working[10], working[15]);
        quarterRound(working[1], working[6], working[11], working[12]);
        quarterRound(working[2], working[7], working[8], working[13]);
        quarterRound(working[3], working[4], working[9], working[14]);
    }

    for (int i = 0; i < 16; ++i) working[i] += state[i];
    std::memcpy(out, working, 64);
}

}  // namespace

void chacha8Xor(const std::uint8_t key[kChaCha8KeySize], const std::uint8_t nonce[kChaCha8NonceSize],
                std::uint32_t counter, const std::uint8_t* in, std::uint8_t* out, std::size_t len) {
    std::uint8_t block[64];
    std::size_t offset = 0;
    while (offset < len) {
        chacha8Block(key, nonce, counter, block);
        std::size_t take = (len - offset < 64) ? (len - offset) : 64;
        for (std::size_t i = 0; i < take; ++i) out[offset + i] = in[offset + i] ^ block[i];
        offset += take;
        ++counter;
    }
}

}  // namespace annpocketdb::encryption
