#include "annpocketdb/encryption/page_cipher.hpp"

#include <cstdlib>
#include <cstring>

#include "monocypher.h"

namespace annpocketdb::encryption {

Status deriveKeyFromPassphrase(std::string_view passphrase, ByteView salt, Key& outKey,
                               const KdfParams& params) {
    if (salt.empty()) return Status::InvalidArgument("Argon2id salt must not be empty");
    if (passphrase.empty()) return Status::InvalidArgument("passphrase must not be empty");

    const std::size_t workBytes = static_cast<std::size_t>(params.memoryKb) * 1024;
    void* workArea = std::malloc(workBytes);
    if (workArea == nullptr) return Status::OutOfMemory("Argon2id work area allocation failed");

    crypto_argon2_config config;
    config.algorithm = CRYPTO_ARGON2_ID;
    config.nb_blocks = params.memoryKb;
    config.nb_passes = params.iterations;
    config.nb_lanes = params.parallelism;

    crypto_argon2_inputs inputs;
    inputs.pass = reinterpret_cast<const std::uint8_t*>(passphrase.data());
    inputs.pass_size = static_cast<std::uint32_t>(passphrase.size());
    inputs.salt = reinterpret_cast<const std::uint8_t*>(salt.data());
    inputs.salt_size = static_cast<std::uint32_t>(salt.size());

    crypto_argon2(outKey.data(), static_cast<std::uint32_t>(outKey.size()), workArea, config, inputs,
                 crypto_argon2_no_extras);

    crypto_wipe(workArea, workBytes);
    std::free(workArea);
    return Status::OK();
}

void buildPageNonce(PageId pageId, std::uint64_t counter, std::uint8_t outNonce[kNonceSize]) {
    std::memset(outNonce, 0, kNonceSize);
    std::memcpy(outNonce, &pageId, sizeof(pageId));
    std::memcpy(outNonce + sizeof(pageId), &counter, sizeof(counter));
    // Remaining 8 bytes stay zero -- (pageId, counter) alone already
    // guarantees a fresh nonce per key (see header comment).
}

void encryptPage(const Key& key, PageId pageId, std::uint64_t counter, ByteView plaintext,
                 std::uint8_t* outCipher, Mac& outMac) {
    std::uint8_t nonce[kNonceSize];
    buildPageNonce(pageId, counter, nonce);
    crypto_aead_lock(outCipher, outMac.data(), key.data(), nonce, nullptr, 0,
                     reinterpret_cast<const std::uint8_t*>(plaintext.data()), plaintext.size());
}

bool decryptPage(const Key& key, PageId pageId, std::uint64_t counter,
                 const std::uint8_t* cipher, std::size_t cipherLen, const Mac& mac,
                 std::uint8_t* outPlain) {
    std::uint8_t nonce[kNonceSize];
    buildPageNonce(pageId, counter, nonce);
    return crypto_aead_unlock(outPlain, mac.data(), key.data(), nonce, nullptr, 0, cipher, cipherLen) == 0;
}

}  // namespace annpocketdb::encryption
