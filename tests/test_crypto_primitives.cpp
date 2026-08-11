// Crypto suite primitives (Item 7): page_cipher's XChaCha20-Poly1305
// encrypt/decrypt + Argon2id key derivation, and the standalone (explicitly
// weakened, unauthenticated) ChaCha8 fast-peek stream. See
// encryption/page_cipher.hpp / encryption/chacha8.hpp for the full contract
// and warnings.
#include "test_common.hpp"

#include <cstring>
#include <vector>

#include "annpocketdb/encryption/chacha8.hpp"
#include "annpocketdb/encryption/page_cipher.hpp"

using namespace annpocketdb;
using namespace annpocketdb::encryption;

int main() {
    // ---- deriveKeyFromPassphrase: deterministic, salt-sensitive ----
    {
        std::string salt1 = "0123456789abcdef";
        std::string salt2 = "fedcba9876543210";
        KdfParams fastParams;
        fastParams.memoryKb = 8 * 1024;  // keep the test fast; still Argon2id
        fastParams.iterations = 1;
        fastParams.parallelism = 1;

        Key k1{}, k1Again{}, k2{};
        CHECK_OK(deriveKeyFromPassphrase("correct horse battery staple", toByteView(salt1), k1, fastParams));
        CHECK_OK(deriveKeyFromPassphrase("correct horse battery staple", toByteView(salt1), k1Again, fastParams));
        CHECK_OK(deriveKeyFromPassphrase("correct horse battery staple", toByteView(salt2), k2, fastParams));
        CHECK(k1 == k1Again);
        CHECK(!(k1 == k2));
    }

    // ---- encryptPage / decryptPage round trip + tamper detection ----
    {
        Key key{};
        key.fill(0x42);
        constexpr std::size_t kPageSize = 4096;
        std::vector<std::uint8_t> plaintext(kPageSize);
        for (std::size_t i = 0; i < kPageSize; ++i) plaintext[i] = static_cast<std::uint8_t>(i * 7 + 3);

        std::vector<std::uint8_t> cipher(kPageSize);
        Mac mac{};
        encryptPage(key, /*pageId=*/42, /*counter=*/1,
                    ByteView(reinterpret_cast<const std::byte*>(plaintext.data()), kPageSize),
                    cipher.data(), mac);

        // Ciphertext must not equal the plaintext (sanity: it's actually transforming the bytes).
        CHECK(std::memcmp(cipher.data(), plaintext.data(), kPageSize) != 0);

        std::vector<std::uint8_t> decrypted(kPageSize);
        CHECK(decryptPage(key, 42, 1, cipher.data(), kPageSize, mac, decrypted.data()));
        CHECK(decrypted == plaintext);

        // Wrong pageId -> wrong nonce -> MAC fails.
        std::vector<std::uint8_t> attempt(kPageSize);
        CHECK(!decryptPage(key, 43, 1, cipher.data(), kPageSize, mac, attempt.data()));

        // Wrong counter -> wrong nonce -> MAC fails.
        CHECK(!decryptPage(key, 42, 2, cipher.data(), kPageSize, mac, attempt.data()));

        // Bit-flipped ciphertext -> MAC fails.
        std::vector<std::uint8_t> tamperedCipher = cipher;
        tamperedCipher[100] ^= 0x01;
        CHECK(!decryptPage(key, 42, 1, tamperedCipher.data(), kPageSize, mac, attempt.data()));

        // Bit-flipped MAC -> fails.
        Mac tamperedMac = mac;
        tamperedMac[0] ^= 0x01;
        CHECK(!decryptPage(key, 42, 1, cipher.data(), kPageSize, tamperedMac, attempt.data()));

        // Wrong key -> fails.
        Key wrongKey{};
        wrongKey.fill(0x99);
        CHECK(!decryptPage(wrongKey, 42, 1, cipher.data(), kPageSize, mac, attempt.data()));
    }

    // ---- literal ChaCha8: symmetric XOR stream, explicitly unauthenticated ----
    {
        std::uint8_t key[kChaCha8KeySize];
        for (std::size_t i = 0; i < kChaCha8KeySize; ++i) key[i] = static_cast<std::uint8_t>(i);
        std::uint8_t nonce[kChaCha8NonceSize];
        for (std::size_t i = 0; i < kChaCha8NonceSize; ++i) nonce[i] = static_cast<std::uint8_t>(0xA0 + i);

        std::string message = "the quick brown fox jumps over the lazy dog, 8-round style";
        std::vector<std::uint8_t> plain(message.begin(), message.end());
        std::vector<std::uint8_t> cipher(plain.size());
        chacha8Xor(key, nonce, /*counter=*/0, plain.data(), cipher.data(), plain.size());
        CHECK(std::memcmp(cipher.data(), plain.data(), plain.size()) != 0);

        std::vector<std::uint8_t> roundTrip(plain.size());
        chacha8Xor(key, nonce, 0, cipher.data(), roundTrip.data(), cipher.size());
        CHECK(roundTrip == plain);

        // Different counter -> different keystream -> does not recover the message.
        std::vector<std::uint8_t> wrongCounter(plain.size());
        chacha8Xor(key, nonce, 1, cipher.data(), wrongCounter.data(), cipher.size());
        CHECK(wrongCounter != plain);
    }

    TEST_MAIN_END();
}
