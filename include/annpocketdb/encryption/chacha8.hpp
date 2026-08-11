#pragma once
//
// AnnPocketDb :: encryption/chacha8.hpp
//
// Literal ChaCha8: the original Bernstein ChaCha stream cipher reduced to 8
// rounds (4 double-rounds) instead of the standard-strength 20 used by
// encryption::encryptPage's XChaCha20-Poly1305. Requested explicitly as a
// "fast peek" path, for parity with the LMDB fork this project draws
// inspiration from.
//
// ============================================================================
//                                  WARNING
// ============================================================================
// This is NOT a secure cipher. Reduced-round ChaCha has known cryptanalytic
// attacks well below 20 rounds; treat anything produced by this as merely
// obfuscated, never confidential. It is also completely UNAUTHENTICATED --
// no MAC at all, so a bit-flipped ciphertext silently produces corrupted
// plaintext with no detection. Never use this where real confidentiality or
// integrity matters -- use encryptPage()/decryptPage() (page_cipher.hpp) for
// that. This exists solely as an explicit, opt-in, unauthenticated fast path.
// ============================================================================
//
#include <cstddef>
#include <cstdint>

namespace annpocketdb::encryption {

constexpr std::size_t kChaCha8KeySize = 32;
constexpr std::size_t kChaCha8NonceSize = 12;  // IETF-style: 96-bit nonce + 32-bit block counter

// XORs `len` bytes of keystream (starting at block `counter`) into `in`,
// writing to `out` (in == out is fine, for in-place use). Symmetric: the
// same call decrypts ciphertext produced by an earlier call with the same
// key/nonce/counter.
void chacha8Xor(const std::uint8_t key[kChaCha8KeySize], const std::uint8_t nonce[kChaCha8NonceSize],
                std::uint32_t counter, const std::uint8_t* in, std::uint8_t* out, std::size_t len);

}  // namespace annpocketdb::encryption
