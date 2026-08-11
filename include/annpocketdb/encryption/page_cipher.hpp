#pragma once
//
// AnnPocketDb :: encryption/page_cipher.hpp
//
// Page-level authenticated encryption (XChaCha20-Poly1305 via vendored
// Monocypher) plus Argon2id passphrase-based key derivation. See
// core/encryption_sidecar.hpp for where the nonce counter + MAC this
// produces actually gets stored -- never inline in the page itself, since
// that would mean shrinking the B+tree's usable payload capacity for every
// page (encrypted or not).
//
// Nonce construction: XChaCha20's 24-byte nonce is built from
// (pageId, counter) -- deliberately NOT (pageId, txnId), because txnId
// lives *inside* the header this function is encrypting/decrypting, so it
// isn't available before decryption succeeds. `counter` is instead an
// explicit, external, strictly-increasing-per-page value the caller
// (PageManager, via EncryptionSidecar) tracks and bumps on every write to
// that page id -- this is what guarantees the nonce is never reused for two
// different plaintexts under the same key, the one hard requirement
// (X)ChaCha20-Poly1305 depends on for confidentiality.
//
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "annpocketdb/util/status.hpp"
#include "annpocketdb/util/types.hpp"

namespace annpocketdb::encryption {

constexpr std::size_t kKeySize = 32;
constexpr std::size_t kNonceSize = 24;
constexpr std::size_t kMacSize = 16;

using Key = std::array<std::uint8_t, kKeySize>;
using Mac = std::array<std::uint8_t, kMacSize>;

// Argon2id passphrase -> 32-byte key. `salt` should be a fixed, per-database
// random value generated once at creation and stored alongside the database
// (see api::Environment). Tuned to ~64 MiB / 3 passes / 4 lanes, a
// reasonable interactive-use default (mirrors AnnMeta's own KeyDeriver).
struct KdfParams {
    std::uint32_t memoryKb = 65536;
    std::uint32_t iterations = 3;
    std::uint32_t parallelism = 4;
};
Status deriveKeyFromPassphrase(std::string_view passphrase, ByteView salt, Key& outKey,
                               const KdfParams& params = {});

// Exposed mainly for tests; PageManager is the only real caller.
void buildPageNonce(PageId pageId, std::uint64_t counter, std::uint8_t outNonce[kNonceSize]);

// Encrypts exactly `plaintext.size()` bytes (a full raw page image, header
// included) into `outCipher` (same size, caller-owned buffer) plus a
// separate 16-byte MAC.
void encryptPage(const Key& key, PageId pageId, std::uint64_t counter, ByteView plaintext,
                 std::uint8_t* outCipher, Mac& outMac);

// Decrypts `cipher` (cipherLen bytes) + `mac` into `outPlain` (cipherLen
// bytes of caller-owned room). Returns false (outPlain contents then
// unspecified) if the MAC does not verify -- corrupt ciphertext, wrong key,
// or wrong (pageId, counter).
bool decryptPage(const Key& key, PageId pageId, std::uint64_t counter,
                 const std::uint8_t* cipher, std::size_t cipherLen, const Mac& mac,
                 std::uint8_t* outPlain);

}  // namespace annpocketdb::encryption
