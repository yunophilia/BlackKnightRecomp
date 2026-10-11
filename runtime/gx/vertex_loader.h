#pragma once
#include "gx_state.h"
#include <vector>

// Canonical vertex fed to the host vertex shader.
struct HostVertex
{
    float pos[3];
    float nrm[3];
    float bin[3];
    float tan[3];
    uint32_t col[2];        // RGBA8, R in the low byte
    float tex[8][2];
    uint32_t mtx[3];        // packed 8-bit matrix indices: pos,t0,t1,t2 | t3..t6 | t7
};

// Which components the current vertex format provides (part of the VS key).
enum : uint32_t
{
    VC_POS3 = 1 << 0,
    VC_NRM = 1 << 1,
    VC_NBT = 1 << 2,
    VC_COL0 = 1 << 3,
    VC_COL1 = 1 << 4,
    VC_POSMTX = 1 << 5,
    VC_TEX0 = 1 << 8,       // 8 bits: texcoord i present
    VC_TEXMTX0 = 1 << 16,   // 8 bits: per-vertex tex matrix index i present
};

uint32_t VertexLoader_Components(const VertexLayout& l);
// Decodes `count` vertices; returns false for vertices that must be skipped
// (position index 0xFF/0xFFFF) by marking them in `skip`.
void VertexLoader_Decode(const VertexLayout& l, const uint8_t* src, uint32_t count, HostVertex* out, std::vector<uint8_t>& skip);
