#pragma once
#include <cstdint>

// Raw GX register state as written by the command stream.
struct GXState
{
    uint32_t bp[0x100];
    uint32_t cp[0x100];
    uint32_t xfmem[0x1000];     // matrices, lights (0x0000-0x0FFF)
    uint32_t xfregs[0x100];     // 0x1000-0x10FF
    uint32_t bpMaskValue = 0xFFFFFF;   // BP mask (reg 0xFE) for next write
};

extern GXState g_gx;

// Vertex attribute component layout of one vertex as decoded from VCD/VAT.
struct VertexLayout
{
    uint32_t size;
    // per-attribute descriptors
    uint8_t posMtxIdx;      // 0 none, 1 direct
    uint8_t texMtxIdx[8];
    uint8_t pos, nrm, col[2], tex[8];          // 0 none,1 direct,2 idx8,3 idx16
    uint8_t posCnt, posFmt, posShift;
    uint8_t nrmCnt, nrmFmt, nrmIndex3;
    uint8_t colCnt[2], colFmt[2];
    uint8_t texCnt[8], texFmt[8], texShift[8];
    bool byteDequant;
};

void GX_ComputeLayout(int vat, VertexLayout& l);

inline float XFFloat(uint32_t idx)
{
    union { uint32_t u; float f; } c;
    c.u = idx < 0x1000 ? g_gx.xfmem[idx] : g_gx.xfregs[idx - 0x1000];
    return c.f;
}

// CP registers
enum
{
    CP_MATINDEX_A = 0x30,
    CP_MATINDEX_B = 0x40,
    CP_VCD_LO = 0x50,
    CP_VCD_HI = 0x60,
    CP_VAT_A = 0x70,
    CP_VAT_B = 0x80,
    CP_VAT_C = 0x90,
    CP_ARRAY_BASE = 0xA0,
    CP_ARRAY_STRIDE = 0xB0,
};

// XF registers (index into xfregs)
enum
{
    XF_ERROR = 0x00,
    XF_DIAG = 0x01,
    XF_STATE0 = 0x02,
    XF_STATE1 = 0x03,
    XF_CLOCK = 0x04,
    XF_CLIPDISABLE = 0x05,
    XF_SETGPMETRIC = 0x06,
    XF_VTXSPECS = 0x08,
    XF_NUMCOLORS = 0x09,
    XF_AMBIENT0 = 0x0A,
    XF_AMBIENT1 = 0x0B,
    XF_MATERIAL0 = 0x0C,
    XF_MATERIAL1 = 0x0D,
    XF_COLOR0CNTRL = 0x0E,
    XF_COLOR1CNTRL = 0x0F,
    XF_ALPHA0CNTRL = 0x10,
    XF_ALPHA1CNTRL = 0x11,
    XF_DUALTEX = 0x12,
    XF_MATINDEX_A = 0x18,
    XF_MATINDEX_B = 0x19,
    XF_VIEWPORT = 0x1A,     // 6 floats
    XF_PROJECTION = 0x20,   // 6 floats + type
    XF_NUMTEXGENS = 0x3F,
    XF_TEXMTXINFO = 0x40,   // 8
    XF_POSTMTXINFO = 0x50,  // 8
};

// Renderer callbacks (implemented by the D3D backend)
void Render_Draw(int primitive, int vat, uint32_t count, const uint8_t* data, const VertexLayout& layout);
void Render_OnBP(uint32_t reg, uint32_t oldValue, uint32_t newValue);
void Render_OnXF(uint32_t addr, uint32_t count);
void Render_OnCP(uint32_t reg);
void Render_EFBCopy();
void Render_LoadTLUT();
void Render_PreloadTMEM();
void Render_InvalidateTextureCache();
