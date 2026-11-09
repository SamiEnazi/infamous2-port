/*
 * sha1_portable.h -- CommonCrypto's CC_SHA1_* interface on hosts without it
 * (Windows, Linux). Plain FIPS 180-4 SHA-1; used by game_patches.cpp for the
 * RPCS3 "PPU-<sha1>" key. Verified against Python hashlib by
 * tests/sha1/test_sha1.py.
 */
#pragma once
#include <stdint.h>
#include <string.h>

typedef uint32_t CC_LONG;
#define CC_SHA1_DIGEST_LENGTH 20
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; uint32_t n; } CC_SHA1_CTX;

static inline uint32_t ps3w_sha1_rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }

static inline void ps3w_sha1_block(CC_SHA1_CTX* c, const uint8_t* p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = ps3w_sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | (~b & d);           k = 0x5A827999u; }
        else if (i < 40) { f = b ^ cc ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ cc ^ d;                    k = 0xCA62C1D6u; }
        const uint32_t t = ps3w_sha1_rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = ps3w_sha1_rol(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static inline int CC_SHA1_Init(CC_SHA1_CTX* c)
{
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->len = 0; c->n = 0;
    return 1;
}

static inline int CC_SHA1_Update(CC_SHA1_CTX* c, const void* data, CC_LONG len)
{
    const uint8_t* p = (const uint8_t*)data;
    c->len += len;
    while (len) {
        uint32_t take = 64 - c->n;
        if (take > len) take = len;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; len -= take;
        if (c->n == 64) { ps3w_sha1_block(c, c->buf); c->n = 0; }
    }
    return 1;
}

static inline int CC_SHA1_Final(unsigned char* md, CC_SHA1_CTX* c)
{
    const uint64_t bits = c->len * 8;
    const uint8_t one = 0x80, zero = 0;
    CC_SHA1_Update(c, &one, 1);
    while (c->n != 56) CC_SHA1_Update(c, &zero, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    CC_SHA1_Update(c, lb, 8);
    for (int i = 0; i < 5; i++) {
        md[4 * i] = (uint8_t)(c->h[i] >> 24); md[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        md[4 * i + 2] = (uint8_t)(c->h[i] >> 8); md[4 * i + 3] = (uint8_t)c->h[i];
    }
    return 1;
}
