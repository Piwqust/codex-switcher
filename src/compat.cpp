#include "compat.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <climits>
#include <cstring>
#include <new>

#pragma comment(lib, "bcrypt.lib")

namespace Compat {
    namespace {
        using u8 = uint8_t;
        using u16 = uint16_t;
        using u32 = uint32_t;
        using u64 = uint64_t;

        // Codex Switcher's constants (src-tauri/src/commands/account.rs).
        const char kMagic[] = "CSWF";
        const u8 kVersion = 1;
        const size_t kSaltLen = 16, kNonceLen = 24, kHeaderLen = 4 + 1 + kSaltLen + kNonceLen;
        const unsigned kIterations = 210000;
        const char kPassphrase[] = "gT7kQ9mV2xN4pL8sR1dH6zW3cB5yF0uJ_aE7nK2tP9vM4rX1";
        const size_t kMaxJson = 2u << 20, kMaxFile = 8u << 20;

        const u8* Bytes(const std::string& s) { return reinterpret_cast<const u8*>(s.data()); }
        void Wipe(std::string& s) { if (!s.empty()) SecureZeroMemory(&s[0], s.size()); s.clear(); }

        u32 Load32(const u8* p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
        void Store32(u8* p, u32 v) { for (int i = 0; i < 4; i++) p[i] = (u8)(v >> 8 * i); }
        void Store64(u8* p, u64 v) { for (int i = 0; i < 8; i++) p[i] = (u8)(v >> 8 * i); }

        // ---- ChaCha20 (RFC 8439) and HChaCha20 ----

        u32 Rotl(u32 v, int n) { return v << n | v >> (32 - n); }

        void Quarter(u32* x, int a, int b, int c, int d) {
            x[a] += x[b]; x[d] = Rotl(x[d] ^ x[a], 16);
            x[c] += x[d]; x[b] = Rotl(x[b] ^ x[c], 12);
            x[a] += x[b]; x[d] = Rotl(x[d] ^ x[a], 8);
            x[c] += x[d]; x[b] = Rotl(x[b] ^ x[c], 7);
        }

        // Constants, key, then 16 bytes of counter/nonce; 20 rounds.
        void Core(u32 x[16], const u8 key[32], const u8 tail[16]) {
            x[0] = 0x61707865; x[1] = 0x3320646e; x[2] = 0x79622d32; x[3] = 0x6b206574;
            for (int i = 0; i < 8; i++) x[4 + i] = Load32(key + 4 * i);
            for (int i = 0; i < 4; i++) x[12 + i] = Load32(tail + 4 * i);
        }

        void Rounds(u32 x[16]) {
            for (int i = 0; i < 10; i++) {
                Quarter(x, 0, 4, 8, 12); Quarter(x, 1, 5, 9, 13); Quarter(x, 2, 6, 10, 14); Quarter(x, 3, 7, 11, 15);
                Quarter(x, 0, 5, 10, 15); Quarter(x, 1, 6, 11, 12); Quarter(x, 2, 7, 8, 13); Quarter(x, 3, 4, 9, 14);
            }
        }

        void Block(const u8 key[32], u32 counter, const u8 nonce[12], u8 out[64]) {
            u8 tail[16];
            Store32(tail, counter);
            memcpy(tail + 4, nonce, 12);
            u32 s[16], x[16];
            Core(s, key, tail);
            memcpy(x, s, sizeof x);
            Rounds(x);
            for (int i = 0; i < 16; i++) Store32(out + 4 * i, x[i] + s[i]);
            SecureZeroMemory(s, sizeof s);
            SecureZeroMemory(x, sizeof x);
        }

        // HChaCha20: no final addition; the subkey is state words 0..3 and 12..15.
        void HChaCha(const u8 key[32], const u8 nonce[16], u8 out[32]) {
            u32 x[16];
            Core(x, key, nonce);
            Rounds(x);
            for (int i = 0; i < 4; i++) { Store32(out + 4 * i, x[i]); Store32(out + 16 + 4 * i, x[12 + i]); }
            SecureZeroMemory(x, sizeof x);
        }

        void Xor(const u8 key[32], u32 counter, const u8 nonce[12], const u8* in, u8* out, size_t n) {
            u8 ks[64];
            for (size_t off = 0; off < n; off += 64, counter++) {
                Block(key, counter, nonce, ks);
                size_t m = n - off < 64 ? n - off : 64;
                for (size_t i = 0; i < m; i++) out[off + i] = in[off + i] ^ ks[i];
            }
            SecureZeroMemory(ks, sizeof ks);
        }

        // ---- Poly1305, 26-bit limbs (poly1305-donna 32-bit) ----

        class Poly {
            u32 r[5], h[5] = {}, pad[4];
            u8 buf[16] = {};
            size_t used = 0;

            void Blocks(const u8* m, size_t n, u32 hibit) {
                const u32 r0 = r[0], r1 = r[1], r2 = r[2], r3 = r[3], r4 = r[4];
                const u32 s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
                u32 h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4];
                for (; n >= 16; m += 16, n -= 16) {
                    h0 += Load32(m) & 0x3ffffff;
                    h1 += (Load32(m + 3) >> 2) & 0x3ffffff;
                    h2 += (Load32(m + 6) >> 4) & 0x3ffffff;
                    h3 += (Load32(m + 9) >> 6) & 0x3ffffff;
                    h4 += (Load32(m + 12) >> 8) | hibit;
                    u64 d0 = (u64)h0 * r0 + (u64)h1 * s4 + (u64)h2 * s3 + (u64)h3 * s2 + (u64)h4 * s1;
                    u64 d1 = (u64)h0 * r1 + (u64)h1 * r0 + (u64)h2 * s4 + (u64)h3 * s3 + (u64)h4 * s2;
                    u64 d2 = (u64)h0 * r2 + (u64)h1 * r1 + (u64)h2 * r0 + (u64)h3 * s4 + (u64)h4 * s3;
                    u64 d3 = (u64)h0 * r3 + (u64)h1 * r2 + (u64)h2 * r1 + (u64)h3 * r0 + (u64)h4 * s4;
                    u64 d4 = (u64)h0 * r4 + (u64)h1 * r3 + (u64)h2 * r2 + (u64)h3 * r1 + (u64)h4 * r0;
                    u32 c = (u32)(d0 >> 26); h0 = (u32)d0 & 0x3ffffff;
                    d1 += c; c = (u32)(d1 >> 26); h1 = (u32)d1 & 0x3ffffff;
                    d2 += c; c = (u32)(d2 >> 26); h2 = (u32)d2 & 0x3ffffff;
                    d3 += c; c = (u32)(d3 >> 26); h3 = (u32)d3 & 0x3ffffff;
                    d4 += c; c = (u32)(d4 >> 26); h4 = (u32)d4 & 0x3ffffff;
                    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
                    h1 += c;
                }
                h[0] = h0; h[1] = h1; h[2] = h2; h[3] = h3; h[4] = h4;
            }

        public:
            explicit Poly(const u8 key[32]) {
                r[0] = Load32(key) & 0x3ffffff;  // r is clamped as it is split into limbs
                r[1] = (Load32(key + 3) >> 2) & 0x3ffff03;
                r[2] = (Load32(key + 6) >> 4) & 0x3ffc0ff;
                r[3] = (Load32(key + 9) >> 6) & 0x3f03fff;
                r[4] = (Load32(key + 12) >> 8) & 0x00fffff;
                for (int i = 0; i < 4; i++) pad[i] = Load32(key + 16 + 4 * i);
            }
            ~Poly() { SecureZeroMemory(this, sizeof *this); }

            void Update(const u8* m, size_t n) {
                if (used) {
                    size_t take = 16 - used < n ? 16 - used : n;
                    memcpy(buf + used, m, take);
                    used += take; m += take; n -= take;
                    if (used < 16) return;
                    Blocks(buf, 16, 1u << 24);
                    used = 0;
                }
                size_t full = n & ~(size_t)15;
                Blocks(m, full, 1u << 24);
                if (n > full) { memcpy(buf, m + full, n - full); used = n - full; }
            }

            void Finish(u8 tag[16]) {
                if (used) {  // last partial block: a 1 byte after the message, no 2^128 bit
                    buf[used] = 1;
                    memset(buf + used + 1, 0, 15 - used);
                    Blocks(buf, 16, 0);
                }
                u32 h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4], c;
                c = h1 >> 26; h1 &= 0x3ffffff;
                h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
                h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
                h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
                h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
                h1 += c;
                // g = h + 5 - 2^130; take g when h >= p, in constant time.
                u32 g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
                u32 g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
                u32 g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
                u32 g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
                u32 g4 = h4 + c - (1u << 26);
                u32 mask = (g4 >> 31) - 1;
                h0 = (h0 & ~mask) | (g0 & mask);
                h1 = (h1 & ~mask) | (g1 & mask);
                h2 = (h2 & ~mask) | (g2 & mask);
                h3 = (h3 & ~mask) | (g3 & mask);
                h4 = (h4 & ~mask) | (g4 & mask);
                // h mod 2^128, plus s.
                u32 w[4] = { h0 | h1 << 26, h1 >> 6 | h2 << 20, h2 >> 12 | h3 << 14, h3 >> 18 | h4 << 8 };
                u64 f = 0;
                for (int i = 0; i < 4; i++) { f = (u64)w[i] + pad[i] + (f >> 32); Store32(tag + 4 * i, (u32)f); }
            }
        };

        bool Equal16(const u8* a, const u8* b) {
            volatile u8 d = 0;
            for (int i = 0; i < 16; i++) d |= a[i] ^ b[i];
            return d == 0;
        }

        // RFC 8439 §2.8 decryption: verify the tag over aad and ciphertext, then decrypt.
        bool AeadOpen(const u8 key[32], const u8 nonce[12], const std::string& aad, const std::string& sealed, std::string& plain) {
            plain.clear();
            if (sealed.size() < 16) return false;
            const size_t n = sealed.size() - 16;
            const u8* ct = Bytes(sealed);
            static const u8 zeros[16] = {};
            u8 otk[64], lengths[16], tag[16];
            Block(key, 0, nonce, otk);
            bool ok;
            {
                Poly mac(otk);
                mac.Update(Bytes(aad), aad.size());
                mac.Update(zeros, (16 - aad.size() % 16) % 16);
                mac.Update(ct, n);
                mac.Update(zeros, (16 - n % 16) % 16);
                Store64(lengths, aad.size());
                Store64(lengths + 8, n);
                mac.Update(lengths, 16);
                mac.Finish(tag);
                ok = Equal16(tag, ct + n);
            }
            SecureZeroMemory(otk, sizeof otk);
            if (!ok) return false;
            std::string out(n, '\0');
            if (n) Xor(key, 1, nonce, ct, reinterpret_cast<u8*>(&out[0]), n);
            plain.swap(out);
            return true;
        }

        bool XAeadOpen(const std::string& key, const std::string& nonce, const std::string& aad, const std::string& sealed, std::string& plain) {
            plain.clear();
            if (key.size() != 32 || nonce.size() != 24) return false;
            u8 subkey[32], subnonce[12] = {};
            HChaCha(Bytes(key), Bytes(nonce), subkey);
            memcpy(subnonce + 4, Bytes(nonce) + 16, 8);
            bool ok = AeadOpen(subkey, subnonce, aad, sealed, plain);
            SecureZeroMemory(subkey, sizeof subkey);
            return ok;
        }

        // ---- Inflate (RFC 1951), in the manner of zlib's puff.c ----

        struct Malformed {};
        [[noreturn]] void Fail() { throw Malformed(); }

        struct Huffman {
            u16 count[16];    // number of codes of each length
            u16 symbol[288];  // symbols ordered by code
        };

        // Canonical code from code lengths. Returns 0 if complete, > 0 if incomplete, < 0 if over-subscribed.
        int Build(Huffman& h, const u16* length, int n) {
            memset(h.count, 0, sizeof h.count);
            for (int s = 0; s < n; s++) h.count[length[s]]++;
            if (h.count[0] == n) return 0;
            int left = 1;
            for (int len = 1; len < 16; len++) {
                left = (left << 1) - h.count[len];
                if (left < 0) return left;
            }
            u16 offs[16] = {};
            for (int len = 1; len < 15; len++) offs[len + 1] = (u16)(offs[len] + h.count[len]);
            for (int s = 0; s < n; s++)
                if (length[s]) h.symbol[offs[length[s]]++] = (u16)s;
            return left;
        }

        class Inflater {
            const u8* in;
            size_t size, pos = 0;
            u32 bitBuf = 0;
            int bitCount = 0;
            std::string& out;
            size_t limit;

            int Bits(int need) {
                u32 v = bitBuf;
                while (bitCount < need) {
                    if (pos >= size) Fail();
                    v |= (u32)in[pos++] << bitCount;
                    bitCount += 8;
                }
                bitBuf = v >> need;
                bitCount -= need;
                return (int)(v & ((1u << need) - 1));
            }

            int Decode(const Huffman& h) {
                int code = 0, first = 0, index = 0;
                for (int len = 1; len < 16; len++) {
                    code |= Bits(1);
                    int count = h.count[len];
                    if (code - count < first) return h.symbol[index + (code - first)];
                    index += count;
                    first = (first + count) << 1;
                    code <<= 1;
                }
                Fail();  // no such code (incomplete code set)
            }

            void Stored() {
                bitBuf = 0;  // skip to the byte boundary
                bitCount = 0;
                if (size - pos < 4) Fail();
                u32 len = in[pos] | (u32)in[pos + 1] << 8, nlen = in[pos + 2] | (u32)in[pos + 3] << 8;
                pos += 4;
                if (len != (~nlen & 0xffff) || len > size - pos || len > limit - out.size()) Fail();
                out.append(reinterpret_cast<const char*>(in + pos), len);
                pos += len;
            }

            void Codes(const Huffman& lencode, const Huffman& distcode) {
                static const u16 lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
                static const u8 lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
                static const u16 dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                               1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
                static const u8 dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
                for (;;) {
                    int sym = Decode(lencode);
                    if (sym < 256) {
                        if (out.size() >= limit) Fail();
                        out.push_back((char)sym);
                    } else if (sym == 256) {
                        return;
                    } else {
                        sym -= 257;
                        if (sym >= 29) Fail();
                        size_t len = lbase[sym] + Bits(lext[sym]);
                        int d = Decode(distcode);
                        if (d >= 30) Fail();
                        size_t dist = dbase[d] + Bits(dext[d]);
                        if (dist > out.size() || len > limit - out.size()) Fail();
                        for (size_t from = out.size() - dist; len--; from++) out.push_back(out[from]);
                    }
                }
            }

            void Fixed() {
                Huffman lencode, distcode;
                u16 lengths[288];
                int s = 0;
                for (; s < 144; s++) lengths[s] = 8;
                for (; s < 256; s++) lengths[s] = 9;
                for (; s < 280; s++) lengths[s] = 7;
                for (; s < 288; s++) lengths[s] = 8;
                Build(lencode, lengths, 288);
                for (s = 0; s < 30; s++) lengths[s] = 5;
                Build(distcode, lengths, 30);
                Codes(lencode, distcode);
            }

            void Dynamic() {
                static const u8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
                int nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
                if (nlen > 286 || ndist > 30) Fail();
                u16 lengths[286 + 30] = {};
                for (int i = 0; i < ncode; i++) lengths[order[i]] = (u16)Bits(3);
                Huffman lencode, distcode;
                if (Build(lencode, lengths, 19) != 0) Fail();  // the code-length code must be complete
                for (int i = 0; i < nlen + ndist;) {
                    int sym = Decode(lencode);
                    if (sym < 16) { lengths[i++] = (u16)sym; continue; }
                    u16 len = 0;
                    int repeat;
                    if (sym == 16) {
                        if (i == 0) Fail();
                        len = lengths[i - 1];
                        repeat = 3 + Bits(2);
                    } else if (sym == 17) {
                        repeat = 3 + Bits(3);
                    } else {
                        repeat = 11 + Bits(7);
                    }
                    if (i + repeat > nlen + ndist) Fail();
                    while (repeat--) lengths[i++] = len;
                }
                if (lengths[256] == 0) Fail();  // no end-of-block code
                // Incomplete codes are allowed only when they have a single symbol.
                int err = Build(lencode, lengths, nlen);
                if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) Fail();
                err = Build(distcode, lengths + nlen, ndist);
                if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) Fail();
                Codes(lencode, distcode);
            }

        public:
            Inflater(const u8* data, size_t n, std::string& output, size_t max) : in(data), size(n), out(output), limit(max) {}

            // Decodes every block; returns the number of input bytes used (the last one partially).
            size_t Run() {
                int last;
                do {
                    last = Bits(1);
                    switch (Bits(2)) {
                    case 0: Stored(); break;
                    case 1: Fixed(); break;
                    case 2: Dynamic(); break;
                    default: Fail();
                    }
                } while (!last);
                return pos;
            }
        };

        u32 Adler32(const std::string& data) {
            u32 a = 1, b = 0;
            const u8* p = Bytes(data);
            for (size_t n = data.size(); n;) {
                size_t chunk = n < 5552 ? n : 5552;  // the most bytes before b can overflow
                n -= chunk;
                while (chunk--) { a += *p++; b += a; }
                a %= 65521;
                b %= 65521;
            }
            return b << 16 | a;
        }
    }

    bool ZlibInflate(const std::string& in, std::string& out, size_t limit) {
        const u8* p = Bytes(in);
        // CM 8 with a window of at most 32K, a valid check value, no preset dictionary.
        bool header = in.size() >= 2 + 4 && (p[0] & 0x0f) == 8 && (p[0] >> 4) <= 7 && ((p[0] << 8) | p[1]) % 31 == 0 && !(p[1] & 0x20);
        std::string data;
        bool ok = false;
        if (header) {
            try {
                size_t end = 2 + Inflater(p + 2, in.size() - 2, data, limit).Run();
                const u8* t = p + end;
                ok = in.size() - end == 4 && Adler32(data) == ((u32)t[0] << 24 | (u32)t[1] << 16 | (u32)t[2] << 8 | t[3]);
            } catch (const Malformed&) {
            } catch (const std::bad_alloc&) {
            }
        }
        if (ok) out.swap(data);
        else out.clear();
        Wipe(data);
        return ok;
    }

    bool XChaChaPolyOpen(const std::string& key, const std::string& nonce, const std::string& sealed, std::string& plain) {
        return XAeadOpen(key, nonce, std::string(), sealed, plain);
    }

    std::string Pbkdf2Sha256(const std::string& password, const std::string& salt, unsigned iterations, size_t length) {
        std::string out;
        if (!iterations || !length || length > ULONG_MAX || password.size() > ULONG_MAX || salt.size() > ULONG_MAX) return out;
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG))) return out;
        out.assign(length, '\0');
        NTSTATUS status = BCryptDeriveKeyPBKDF2(alg, (PUCHAR)password.data(), (ULONG)password.size(), (PUCHAR)salt.data(), (ULONG)salt.size(),
                                                iterations, (PUCHAR)&out[0], (ULONG)length, 0);
        BCryptCloseAlgorithmProvider(alg, 0);
        if (!BCRYPT_SUCCESS(status)) Wipe(out);
        return out;
    }

    bool OpenCodexSwitcherFile(const std::string& file, std::string& json) {
        json.clear();
        if (file.size() > kMaxFile || file.size() < kHeaderLen + 16 || file.compare(0, 4, kMagic) != 0 || (u8)file[4] != kVersion) return false;
        std::string key = Pbkdf2Sha256(kPassphrase, file.substr(5, kSaltLen), kIterations, 32), compressed;
        bool ok = key.size() == 32 && XChaChaPolyOpen(key, file.substr(5 + kSaltLen, kNonceLen), file.substr(kHeaderLen), compressed) &&
                  ZlibInflate(compressed, json, kMaxJson);
        Wipe(key);
        Wipe(compressed);
        return ok;
    }

    bool OpenCodexSwitcherText(const std::string& text, std::string& json) {
        json.clear();
        std::string s, compressed;
        for (char c : text)
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\v' && c != '\f') s += c;
        if (s.compare(0, 5, "css1.") == 0) s.erase(0, 5);
        bool ok = !s.empty() && Detail::Base64Decode(s, compressed) && ZlibInflate(compressed, json, kMaxJson);
        Wipe(s);
        Wipe(compressed);
        return ok;
    }

    namespace Detail {
        std::string ChaCha20Block(const std::string& key, uint32_t counter, const std::string& nonce) {
            if (key.size() != 32 || nonce.size() != 12) return std::string();
            u8 out[64];
            Block(Bytes(key), counter, Bytes(nonce), out);
            return std::string(reinterpret_cast<const char*>(out), sizeof out);
        }

        std::string HChaCha20(const std::string& key, const std::string& nonce) {
            if (key.size() != 32 || nonce.size() != 16) return std::string();
            u8 out[32];
            HChaCha(Bytes(key), Bytes(nonce), out);
            return std::string(reinterpret_cast<const char*>(out), sizeof out);
        }

        std::string Poly1305(const std::string& key, const std::string& message) {
            if (key.size() != 32) return std::string();
            u8 tag[16];
            Poly mac(Bytes(key));
            mac.Update(Bytes(message), message.size());
            mac.Finish(tag);
            return std::string(reinterpret_cast<const char*>(tag), sizeof tag);
        }

        bool ChaChaPolyOpen(const std::string& key, const std::string& nonce, const std::string& aad, const std::string& sealed, std::string& plain) {
            if (key.size() != 32 || nonce.size() != 12) { plain.clear(); return false; }
            return AeadOpen(Bytes(key), Bytes(nonce), aad, sealed, plain);
        }

        bool XChaChaPolyOpen(const std::string& key, const std::string& nonce, const std::string& aad, const std::string& sealed, std::string& plain) {
            return XAeadOpen(key, nonce, aad, sealed, plain);
        }

        // Standard (+/) or URL-safe (-_) alphabet, not mixed; padding optional but, if present, complete.
        bool Base64Decode(const std::string& text, std::string& out) {
            out.clear();
            size_t n = text.size();
            while (n && text[n - 1] == '=') n--;
            if (text.size() - n > 2 || (n < text.size() && text.size() % 4) || n % 4 == 1) return false;
            std::string r;
            r.reserve(n / 4 * 3 + 2);
            u32 acc = 0;
            int bits = 0;
            bool standard = false, urlSafe = false;
            for (size_t i = 0; i < n; i++) {
                char c = text[i];
                u32 v;
                if (c >= 'A' && c <= 'Z') v = (u32)(c - 'A');
                else if (c >= 'a' && c <= 'z') v = (u32)(c - 'a' + 26);
                else if (c >= '0' && c <= '9') v = (u32)(c - '0' + 52);
                else if (c == '+' || c == '/') { v = c == '+' ? 62 : 63; standard = true; }
                else if (c == '-' || c == '_') { v = c == '-' ? 62 : 63; urlSafe = true; }
                else return false;
                acc = (acc << 6 | v) & 0xffffff;
                bits += 6;
                if (bits >= 8) { bits -= 8; r.push_back((char)(acc >> bits)); }
            }
            if (standard && urlSafe) return false;
            out.swap(r);
            return true;
        }
    }
}
