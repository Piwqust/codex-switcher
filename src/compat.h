// Reading the account backups of Codex Switcher (the Rust app) so they can be imported: its
// encrypted full file ("CSWF") and its "css1." slim text. Self-contained: zlib inflate and
// XChaCha20-Poly1305 are implemented here, PBKDF2 comes from Windows CNG (bcrypt.lib).
#pragma once
#include <cstdint>
#include <string>

namespace Compat {
    // RFC 1950 zlib stream (2-byte header, deflate RFC 1951 body, Adler-32 trailer, verified).
    // Returns false on any malformed input or if the output would exceed `limit` bytes.
    bool ZlibInflate(const std::string& in, std::string& out, size_t limit = 8u << 20);
    // XChaCha20-Poly1305 AEAD decryption (draft-irtf-cfrg-xchacha), empty associated data.
    // key 32 bytes, nonce 24 bytes, `sealed` = ciphertext followed by the 16-byte tag.
    // False if sizes are wrong or the tag doesn't verify (no plaintext is returned then).
    bool XChaChaPolyOpen(const std::string& key, const std::string& nonce, const std::string& sealed, std::string& plain);
    // PBKDF2-HMAC-SHA256 through CNG. Empty on failure.
    std::string Pbkdf2Sha256(const std::string& password, const std::string& salt, unsigned iterations, size_t length);
    // Codex Switcher's encrypted full backup: "CSWF", version 1, salt(16), nonce(24), then
    // XChaCha20-Poly1305 ciphertext||tag of the zlib-compressed JSON, under a key derived from
    // the app's built-in passphrase. Returns the JSON text (UTF-8, at most 2 MB) or false.
    bool OpenCodexSwitcherFile(const std::string& file, std::string& json);
    // Codex Switcher's slim text export: "css1." + base64 of zlib-compressed JSON. Whitespace is
    // ignored and the prefix is optional (as in the original); standard or URL-safe alphabet,
    // with or without padding. Returns the JSON text; the caller checks its "v" field.
    bool OpenCodexSwitcherText(const std::string& text, std::string& json);

    // Building blocks, exposed for the known-answer tests. Wrong sizes give an empty result / false.
    namespace Detail {
        std::string ChaCha20Block(const std::string& key, uint32_t counter, const std::string& nonce);  // 32 + 12 -> 64 bytes
        std::string HChaCha20(const std::string& key, const std::string& nonce);                          // 32 + 16 -> 32 bytes
        std::string Poly1305(const std::string& key, const std::string& message);                         // 32 -> 16 bytes
        // RFC 8439 AEAD_CHACHA20_POLY1305 (12-byte nonce) and its XChaCha form (24-byte nonce), with AAD.
        bool ChaChaPolyOpen(const std::string& key, const std::string& nonce, const std::string& aad, const std::string& sealed, std::string& plain);
        bool XChaChaPolyOpen(const std::string& key, const std::string& nonce, const std::string& aad, const std::string& sealed, std::string& plain);
        bool Base64Decode(const std::string& text, std::string& out);
    }
}
