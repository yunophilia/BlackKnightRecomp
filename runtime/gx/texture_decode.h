#pragma once
#include <cstdint>

enum GXTexFmt : uint32_t
{
    TF_I4 = 0, TF_I8 = 1, TF_IA4 = 2, TF_IA8 = 3, TF_RGB565 = 4, TF_RGB5A3 = 5, TF_RGBA8 = 6,
    TF_C4 = 8, TF_C8 = 9, TF_C14X2 = 10, TF_CMPR = 14,
};

// Encoded byte size of a w*h texture (rounded to whole blocks)
uint32_t GXTex_Size(uint32_t fmt, uint32_t w, uint32_t h);
uint32_t GXTex_BlockW(uint32_t fmt);
uint32_t GXTex_BlockH(uint32_t fmt);
// Decode to RGBA8 (R in byte 0). `tlut` points at big-endian palette entries for CI formats.
void GXTex_Decode(uint32_t* dst, const uint8_t* src, uint32_t w, uint32_t h, uint32_t fmt, const uint8_t* tlut, uint32_t tlutFmt);
