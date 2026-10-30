// DXBC container fixups for bytecode from Wine's d3dcompiler (see dxbc_fixup.h).
#include "dxbc_fixup.h"

#include <cstring>
#include <vector>

// ---------------------------------------------------------------------- checksum
static uint32_t Rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

static void Md5Block(uint32_t s[4], const uint32_t m[16])
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const int R[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3];
    for (int i = 0; i < 64; i++)
    {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        uint32_t t = d;
        d = c;
        c = b;
        b = b + Rol(a + f + K[i] + m[g], R[(i / 16) * 4 + (i & 3)]);
        a = t;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
}

// MD5 over the container after the checksum field, with DXBC's own final block layout:
// the bit count goes first in the last block and (bytes * 2 + 1) last, instead of MD5's
// 64-bit length at the end.
void DxbcChecksum(const void* data, size_t size, uint32_t out[4])
{
    const uint8_t* p = (const uint8_t*)data + 20;
    uint32_t n = size > 20 ? (uint32_t)(size - 20) : 0;
    uint32_t s[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    uint32_t m[16];
    uint32_t full = n & ~63u;
    for (uint32_t i = 0; i < full; i += 64)
    {
        memcpy(m, p + i, 64);
        Md5Block(s, m);
    }
    uint32_t rest = n - full;
    uint8_t blk[64] = {};
    if (rest < 56)
    {
        uint32_t bits = n * 8;
        memcpy(blk, &bits, 4);
        memcpy(blk + 4, p + full, rest);
        blk[4 + rest] = 0x80;
    }
    else
    {
        memcpy(blk, p + full, rest);
        blk[rest] = 0x80;
        memcpy(m, blk, 64);
        Md5Block(s, m);
        memset(blk, 0, 64);
        uint32_t bits = n * 8;
        memcpy(blk, &bits, 4);
    }
    uint32_t tail = n * 2 + 1;
    memcpy(blk + 60, &tail, 4);
    memcpy(m, blk, 64);
    Md5Block(s, m);
    memcpy(out, s, 16);
}

// ---------------------------------------------------------------------- declaration order
static uint32_t Rd(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

bool FixDxbcDeclarationOrder(void* data, size_t size)
{
    uint8_t* d = (uint8_t*)data;
    if (size < 32 || memcmp(d, "DXBC", 4) != 0 || Rd(d + 24) > size) return false;
    uint32_t chunks = Rd(d + 28);
    if (chunks > (size - 32) / 4) return false;
    for (uint32_t c = 0; c < chunks; c++)
    {
        uint32_t off = Rd(d + 32 + c * 4);
        if (off > size - 8) return false;
        if (memcmp(d + off, "SHDR", 4) != 0 && memcmp(d + off, "SHEX", 4) != 0) continue;
        uint32_t bytes = Rd(d + off + 4);
        if (bytes > size - off - 8 || bytes < 8) return false;
        uint8_t* code = d + off + 8;
        if ((Rd(code) >> 16) != 0) return false;   // program type 0: pixel shader
        uint32_t count = Rd(code + 4);
        if (count > bytes / 4) return false;

        // instruction boundaries (dword index of each instruction)
        std::vector<uint32_t> start;
        uint32_t lastInput = ~0u;
        for (uint32_t i = 2; i < count;)
        {
            uint32_t tok = Rd(code + i * 4);
            uint32_t op = tok & 0x7FF;
            uint32_t len = op == 0x35 ? (i + 1 < count ? Rd(code + i * 4 + 4) : 0) : (tok >> 24) & 0x7F;
            if (len == 0 || len > count - i) return false;
            if (op >= 0x5F && op <= 0x64) lastInput = (uint32_t)start.size();   // dcl_input*
            start.push_back(i);
            i += len;
        }
        if (lastInput == ~0u) return false;
        start.push_back(count);

        // dcl_output* that come before the last input move to just after it
        std::vector<uint32_t> before, moved;
        for (uint32_t k = 0; k <= lastInput; k++)
        {
            uint32_t op = Rd(code + start[k] * 4) & 0x7FF;
            (op >= 0x65 && op <= 0x67 ? moved : before).push_back(k);
        }
        if (moved.empty()) return false;
        std::vector<uint8_t> out;
        out.reserve((start[lastInput + 1] - 2) * 4);
        for (auto* list : { &before, &moved })
            for (uint32_t k : *list)
                out.insert(out.end(), code + start[k] * 4, code + start[k + 1] * 4);
        memcpy(code + 8, out.data(), out.size());

        uint32_t sum[4];
        DxbcChecksum(d, Rd(d + 24), sum);
        memcpy(d + 4, sum, 16);
        return true;
    }
    return false;
}
