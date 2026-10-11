#include "texture_decode.h"
#include <cstring>

static inline uint16_t BE16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t RGBA(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | (g << 8) | (b << 16) | (a << 24); }

uint32_t GXTex_BlockW(uint32_t fmt)
{
    switch (fmt)
    {
    case TF_I4: case TF_C4: case TF_CMPR: case TF_I8: case TF_IA4: case TF_C8: return 8;
    default: return 4;
    }
}

uint32_t GXTex_BlockH(uint32_t fmt)
{
    switch (fmt)
    {
    case TF_I4: case TF_C4: case TF_CMPR: return 8;
    default: return 4;
    }
}

static uint32_t BytesPerBlock(uint32_t fmt)
{
    switch (fmt)
    {
    case TF_RGBA8: return 64;
    default: return 32;
    }
}

uint32_t GXTex_Size(uint32_t fmt, uint32_t w, uint32_t h)
{
    uint32_t bw = GXTex_BlockW(fmt), bh = GXTex_BlockH(fmt);
    return ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * BytesPerBlock(fmt);
}

static inline uint32_t Decode565(uint16_t v)
{
    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return RGBA((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255);
}

static inline uint32_t Decode5A3(uint16_t v)
{
    if (v & 0x8000)
    {
        uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        return RGBA((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255);
    }
    uint32_t a = (v >> 12) & 7, r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
    return RGBA(r * 17, g * 17, b * 17, (a << 5) | (a << 2) | (a >> 1));
}

static inline uint32_t DecodeIA8(uint16_t v)
{
    uint32_t a = v >> 8, i = v & 0xFF;
    return RGBA(i, i, i, a);
}

static inline uint32_t DecodePal(const uint8_t* tlut, uint32_t idx, uint32_t tlutFmt)
{
    uint16_t v = BE16(tlut + idx * 2);
    switch (tlutFmt)
    {
    case 0: return DecodeIA8(v);
    case 1: return Decode565(v);
    default: return Decode5A3(v);
    }
}

void GXTex_Decode(uint32_t* dst, const uint8_t* src, uint32_t w, uint32_t h, uint32_t fmt, const uint8_t* tlut, uint32_t tlutFmt)
{
    uint32_t bw = GXTex_BlockW(fmt), bh = GXTex_BlockH(fmt);
    uint32_t bpr = (w + bw - 1) / bw;
    uint32_t bcols = (h + bh - 1) / bh;
    const uint8_t* p = src;
    auto put = [&](uint32_t x, uint32_t y, uint32_t c) { if (x < w && y < h) dst[y * w + x] = c; };
    for (uint32_t by = 0; by < bcols; by++)
    {
        for (uint32_t bx = 0; bx < bpr; bx++)
        {
            uint32_t x0 = bx * bw, y0 = by * bh;
            switch (fmt)
            {
            case TF_I4:
                for (uint32_t y = 0; y < 8; y++)
                    for (uint32_t x = 0; x < 8; x += 2)
                    {
                        uint8_t v = *p++;
                        uint32_t i0 = (v >> 4) * 17, i1 = (v & 15) * 17;
                        put(x0 + x, y0 + y, RGBA(i0, i0, i0, i0));
                        put(x0 + x + 1, y0 + y, RGBA(i1, i1, i1, i1));
                    }
                break;
            case TF_I8:
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 8; x++)
                    {
                        uint32_t i = *p++;
                        put(x0 + x, y0 + y, RGBA(i, i, i, i));
                    }
                break;
            case TF_IA4:
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 8; x++)
                    {
                        uint8_t v = *p++;
                        uint32_t a = (v >> 4) * 17, i = (v & 15) * 17;
                        put(x0 + x, y0 + y, RGBA(i, i, i, a));
                    }
                break;
            case TF_IA8:
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++, p += 2)
                        put(x0 + x, y0 + y, DecodeIA8(BE16(p)));
                break;
            case TF_RGB565:
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++, p += 2)
                        put(x0 + x, y0 + y, Decode565(BE16(p)));
                break;
            case TF_RGB5A3:
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++, p += 2)
                        put(x0 + x, y0 + y, Decode5A3(BE16(p)));
                break;
            case TF_RGBA8:
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++)
                    {
                        uint32_t k = (y * 4 + x) * 2;
                        uint32_t a = p[k], r = p[k + 1], g = p[32 + k], b = p[32 + k + 1];
                        put(x0 + x, y0 + y, RGBA(r, g, b, a));
                    }
                p += 64;
                break;
            case TF_C4:
                for (uint32_t y = 0; y < 8; y++)
                    for (uint32_t x = 0; x < 8; x += 2)
                    {
                        uint8_t v = *p++;
                        put(x0 + x, y0 + y, DecodePal(tlut, v >> 4, tlutFmt));
                        put(x0 + x + 1, y0 + y, DecodePal(tlut, v & 15, tlutFmt));
                    }
                break;
            case TF_C8:
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 8; x++)
                        put(x0 + x, y0 + y, DecodePal(tlut, *p++, tlutFmt));
                break;
            case TF_C14X2:
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++, p += 2)
                        put(x0 + x, y0 + y, DecodePal(tlut, BE16(p) & 0x3FFF, tlutFmt));
                break;
            case TF_CMPR:
                for (uint32_t sub = 0; sub < 4; sub++)
                {
                    uint32_t sx = x0 + (sub & 1) * 4, sy = y0 + (sub >> 1) * 4;
                    uint16_t c0 = BE16(p), c1 = BE16(p + 2);
                    uint32_t pal[4];
                    pal[0] = Decode565(c0);
                    pal[1] = Decode565(c1);
                    uint32_t r0 = pal[0] & 0xFF, g0 = (pal[0] >> 8) & 0xFF, b0 = (pal[0] >> 16) & 0xFF;
                    uint32_t r1 = pal[1] & 0xFF, g1 = (pal[1] >> 8) & 0xFF, b1 = (pal[1] >> 16) & 0xFF;
                    if (c0 > c1)
                    {
                        pal[2] = RGBA((r0 * 5 + r1 * 3) >> 3, (g0 * 5 + g1 * 3) >> 3, (b0 * 5 + b1 * 3) >> 3, 255);
                        pal[3] = RGBA((r0 * 3 + r1 * 5) >> 3, (g0 * 3 + g1 * 5) >> 3, (b0 * 3 + b1 * 5) >> 3, 255);
                    }
                    else
                    {
                        pal[2] = RGBA((r0 + r1) >> 1, (g0 + g1) >> 1, (b0 + b1) >> 1, 255);
                        pal[3] = RGBA((r0 + r1) >> 1, (g0 + g1) >> 1, (b0 + b1) >> 1, 0);
                    }
                    for (uint32_t y = 0; y < 4; y++)
                    {
                        uint8_t bits = p[4 + y];
                        for (uint32_t x = 0; x < 4; x++)
                            put(sx + x, sy + y, pal[(bits >> (6 - 2 * x)) & 3]);
                    }
                    p += 8;
                }
                break;
            default:
                // unknown format: magenta
                for (uint32_t y = 0; y < bh; y++)
                    for (uint32_t x = 0; x < bw; x++)
                        put(x0 + x, y0 + y, RGBA(255, 0, 255, 255));
                p += BytesPerBlock(fmt);
                break;
            }
        }
    }
}
