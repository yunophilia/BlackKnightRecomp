// SHA-1 (FIPS 180-4), plain C++.
#include "../sha1.h"

#include <algorithm>
#include <cstring>

namespace
{
struct State
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint8_t block[64];
    size_t used = 0;
    uint64_t bytes = 0;
};

inline uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void Compress(State& s, const uint8_t* p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3], e = s.h[4];
    for (int i = 0; i < 80; i++)
    {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = Rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = Rol(b, 30);
        b = a;
        a = t;
    }
    s.h[0] += a;
    s.h[1] += b;
    s.h[2] += c;
    s.h[3] += d;
    s.h[4] += e;
}
}   // namespace

Sha1::Sha1() : impl(new State()) {}
Sha1::~Sha1() { delete (State*)impl; }

void Sha1::Update(const void* data, size_t len)
{
    State& s = *(State*)impl;
    const uint8_t* p = (const uint8_t*)data;
    s.bytes += len;
    if (s.used)
    {
        size_t n = std::min(len, 64 - s.used);
        memcpy(s.block + s.used, p, n);
        s.used += n;
        p += n;
        len -= n;
        if (s.used < 64) return;
        Compress(s, s.block);
        s.used = 0;
    }
    for (; len >= 64; p += 64, len -= 64) Compress(s, p);
    memcpy(s.block, p, len);
    s.used = len;
}

void Sha1::Final(uint8_t out[20])
{
    State& s = *(State*)impl;
    uint64_t bits = s.bytes * 8;
    uint8_t pad[72] = { 0x80 };
    size_t padLen = (s.used < 56 ? 56 : 120) - s.used;
    for (int i = 0; i < 8; i++) pad[padLen + i] = (uint8_t)(bits >> (56 - 8 * i));
    Update(pad, padLen + 8);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(s.h[i] >> (24 - 8 * j));
}

bool Sha1_Hash(const void* data, size_t len, uint8_t out[20])
{
    Sha1 h;
    h.Update(data, len);
    h.Final(out);
    return true;
}
