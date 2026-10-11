#pragma once
#include <cstdint>
#include <cstring>
#include <string>

// Everything that changes the generated vertex shader.
struct VSKey
{
    uint32_t components;
    uint32_t numTexGens;
    uint32_t dualTex;
    uint32_t texMtxInfo[8];
    uint32_t postMtxInfo[8];
    uint32_t chan[4];          // color0, color1, alpha0, alpha1 control registers
    uint32_t numColorChans;
    bool operator==(const VSKey& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};

// Everything that changes the generated pixel shader.
struct PSKey
{
    uint32_t numTevStages;
    uint32_t numTexGens;
    uint32_t numIndStages;
    uint32_t colorComb[16];
    uint32_t alphaComb[16];
    uint32_t tevOrder[8];
    uint32_t tevInd[16];
    uint32_t ksel[8];
    uint32_t indOrder;
    uint32_t alphaFunc;        // comp0/comp1/logic (refs are constants)
    uint32_t zTexType;         // type | op << 2
    uint32_t fogType;          // fsel | proj << 3 | range << 4
    uint32_t pixelFormat;
    uint32_t dstAlphaEnable;
    uint32_t perPixelDepth;    // ztexture or late z
    uint32_t texIsDepth;       // bit per texmap: sample from depth copy (unused for now)
    bool operator==(const PSKey& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};

std::string GenerateVertexShader(const VSKey& key);
std::string GeneratePixelShader(const PSKey& key);

// Constant buffer layouts shared with the generator
struct alignas(16) VSConstants
{
    float proj[4][4];
    float depth[4];            // x = farZ/2^24, y = zRange/2^24, z = flip x sign, w = flip y sign
    int32_t materials[4][4];   // amb0, amb1, mat0, mat1 (0..255)
    float pixelOffset[4];      // xy: subpixel correction in clip units
};

struct alignas(16) PSConstants
{
    int32_t colors[4][4];      // prev/c0/c1/c2 initial register values
    int32_t konst[4][4];
    int32_t alphaRef[4];       // ref0, ref1, dstAlpha value
    int32_t texDim[8][4];      // w, h, 0, 0
    int32_t indTexScale[4][4]; // per ind stage: shift s, shift t
    int32_t indTexMtx[6][4];   // 3 matrices x 2 rows: m0, m1, m2, scale shift
    int32_t zBias[2][4];       // ztexture: [0] = component weights, [1].w = bias
    float fogF[4];             // a, c, center, viewport width
    int32_t fogI[4];           // 0, b_mag, 0, b_shift
    int32_t fogColor[4];
    float fogRange[3][4];
    float depthParams[4];      // x = farZ, y = zRange (24-bit units)
};
