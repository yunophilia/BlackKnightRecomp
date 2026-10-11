// GX vertex stream -> HostVertex
#include "vertex_loader.h"
#include "../kernel/memory.h"

#include <cstring>

static inline uint16_t BE16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t BE32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static const uint8_t kCompSize[8] = { 1, 1, 2, 2, 4, 0, 0, 0 };
static const uint8_t kColorSize[8] = { 2, 3, 4, 2, 3, 4, 0, 0 };

uint32_t VertexLoader_Components(const VertexLayout& l)
{
    uint32_t c = 0;
    if (l.posCnt) c |= VC_POS3;
    if (l.nrm) c |= VC_NRM;
    if (l.nrm && l.nrmCnt) c |= VC_NBT;
    if (l.col[0]) c |= VC_COL0;
    if (l.col[1]) c |= VC_COL1;
    if (l.posMtxIdx) c |= VC_POSMTX;
    for (int i = 0; i < 8; i++)
    {
        if (l.tex[i]) c |= VC_TEX0 << i;
        if (l.texMtxIdx[i]) c |= VC_TEXMTX0 << i;
    }
    return c;
}

static inline float ReadComp(const uint8_t* p, uint32_t fmt, float scale)
{
    switch (fmt)
    {
    case 0: return (float)p[0] * scale;
    case 1: return (float)(int8_t)p[0] * scale;
    case 2: return (float)BE16(p) * scale;
    case 3: return (float)(int16_t)BE16(p) * scale;
    default:
    {
        uint32_t u = BE32(p);
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
    }
}

static inline uint32_t ReadColor(const uint8_t* p, uint32_t fmt)
{
    uint32_t r, g, b, a;
    switch (fmt)
    {
    case 0: { uint16_t v = BE16(p); r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31; r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2); a = 255; break; }
    case 1: r = p[0]; g = p[1]; b = p[2]; a = 255; break;
    case 2: r = p[0]; g = p[1]; b = p[2]; a = 255; break;
    case 3: { uint16_t v = BE16(p); r = ((v >> 12) & 15) * 17; g = ((v >> 8) & 15) * 17; b = ((v >> 4) & 15) * 17; a = (v & 15) * 17; break; }
    case 4: { uint32_t v = ((uint32_t)p[0] << 16) | (p[1] << 8) | p[2]; r = (v >> 18) & 63; g = (v >> 12) & 63; b = (v >> 6) & 63; a = v & 63;
              r = (r << 2) | (r >> 4); g = (g << 2) | (g >> 4); b = (b << 2) | (b >> 4); a = (a << 2) | (a >> 4); break; }
    default: r = p[0]; g = p[1]; b = p[2]; a = p[3]; break;
    }
    return r | (g << 8) | (b << 16) | (a << 24);
}

// Returns pointer to attribute data (direct: in stream; indexed: in array)
static inline const uint8_t* AttrPtr(uint8_t type, const uint8_t*& s, int array, uint32_t directSize, bool* nullIndex)
{
    const uint8_t* p;
    if (type == 1)
    {
        p = s;
        s += directSize;
        return p;
    }
    uint32_t idx;
    if (type == 2)
    {
        idx = *s++;
        if (nullIndex) *nullIndex = idx == 0xFF;
    }
    else
    {
        idx = BE16(s);
        s += 2;
        if (nullIndex) *nullIndex = idx == 0xFFFF;
    }
    uint32_t base = g_gx.cp[CP_ARRAY_BASE + array];
    uint32_t stride = g_gx.cp[CP_ARRAY_STRIDE + array];
    return PhysPtr(base + idx * stride);
}

void VertexLoader_Decode(const VertexLayout& l, const uint8_t* src, uint32_t count, HostVertex* out, std::vector<uint8_t>& skip)
{
    uint32_t matA = g_gx.xfregs[XF_MATINDEX_A];
    uint32_t matB = g_gx.xfregs[XF_MATINDEX_B];
    uint8_t defMtx[9] = {
        (uint8_t)(matA & 63), (uint8_t)((matA >> 6) & 63), (uint8_t)((matA >> 12) & 63), (uint8_t)((matA >> 18) & 63),
        (uint8_t)((matA >> 24) & 63), (uint8_t)(matB & 63), (uint8_t)((matB >> 6) & 63), (uint8_t)((matB >> 12) & 63),
        (uint8_t)((matB >> 18) & 63),
    };
    float posScale = (l.posFmt < 4) ? 1.0f / (float)(1u << l.posShift) : 1.0f;
    float nrmScale = (l.nrmFmt == 1 || l.nrmFmt == 0) ? 1.0f / 64.0f : (l.nrmFmt < 4 ? 1.0f / 16384.0f : 1.0f);
    float texScale[8];
    for (int i = 0; i < 8; i++) texScale[i] = (l.texFmt[i] < 4) ? 1.0f / (float)(1u << l.texShift[i]) : 1.0f;
    uint32_t posSize = kCompSize[l.posFmt] * (l.posCnt ? 3 : 2);
    uint32_t nrmCompSize = kCompSize[l.nrmFmt];
    skip.assign(count, 0);

    const uint8_t* s = src;
    for (uint32_t v = 0; v < count; v++)
    {
        HostVertex& o = out[v];
        uint8_t mtx[9];
        memcpy(mtx, defMtx, 9);
        if (l.posMtxIdx) mtx[0] = *s++ & 63;
        for (int i = 0; i < 8; i++)
            if (l.texMtxIdx[i]) mtx[1 + i] = *s++ & 63;

        // position
        bool nullIdx = false;
        if (l.pos)
        {
            const uint8_t* p = AttrPtr(l.pos, s, 0, posSize, &nullIdx);
            uint32_t cs = kCompSize[l.posFmt];
            o.pos[0] = ReadComp(p, l.posFmt, posScale);
            o.pos[1] = ReadComp(p + cs, l.posFmt, posScale);
            o.pos[2] = l.posCnt ? ReadComp(p + 2 * cs, l.posFmt, posScale) : 0.0f;
            if (nullIdx && l.pos >= 2) skip[v] = 1;
        }
        else
            o.pos[0] = o.pos[1] = o.pos[2] = 0;

        // normal / binormal / tangent
        if (l.nrm)
        {
            if (l.nrm == 1)
            {
                const uint8_t* p = s;
                uint32_t n = l.nrmCnt ? 9 : 3;
                s += nrmCompSize * n;
                for (int k = 0; k < 3; k++) o.nrm[k] = ReadComp(p + k * nrmCompSize, l.nrmFmt, nrmScale);
                if (l.nrmCnt)
                {
                    for (int k = 0; k < 3; k++) o.bin[k] = ReadComp(p + (3 + k) * nrmCompSize, l.nrmFmt, nrmScale);
                    for (int k = 0; k < 3; k++) o.tan[k] = ReadComp(p + (6 + k) * nrmCompSize, l.nrmFmt, nrmScale);
                }
            }
            else
            {
                uint32_t stride = g_gx.cp[CP_ARRAY_STRIDE + 1];
                uint32_t base = g_gx.cp[CP_ARRAY_BASE + 1];
                auto idx = [&]() -> uint32_t {
                    if (l.nrm == 2) return *s++;
                    uint32_t x = BE16(s);
                    s += 2;
                    return x;
                };
                if (l.nrmCnt && l.nrmIndex3)
                {
                    uint32_t i0 = idx(), i1 = idx(), i2 = idx();
                    const uint8_t* p0 = PhysPtr(base + i0 * stride);
                    const uint8_t* p1 = PhysPtr(base + i1 * stride) + 3 * nrmCompSize;
                    const uint8_t* p2 = PhysPtr(base + i2 * stride) + 6 * nrmCompSize;
                    for (int k = 0; k < 3; k++) o.nrm[k] = ReadComp(p0 + k * nrmCompSize, l.nrmFmt, nrmScale);
                    for (int k = 0; k < 3; k++) o.bin[k] = ReadComp(p1 + k * nrmCompSize, l.nrmFmt, nrmScale);
                    for (int k = 0; k < 3; k++) o.tan[k] = ReadComp(p2 + k * nrmCompSize, l.nrmFmt, nrmScale);
                }
                else
                {
                    const uint8_t* p = PhysPtr(base + idx() * stride);
                    for (int k = 0; k < 3; k++) o.nrm[k] = ReadComp(p + k * nrmCompSize, l.nrmFmt, nrmScale);
                    if (l.nrmCnt)
                    {
                        for (int k = 0; k < 3; k++) o.bin[k] = ReadComp(p + (3 + k) * nrmCompSize, l.nrmFmt, nrmScale);
                        for (int k = 0; k < 3; k++) o.tan[k] = ReadComp(p + (6 + k) * nrmCompSize, l.nrmFmt, nrmScale);
                    }
                }
            }
        }

        // colors
        for (int c = 0; c < 2; c++)
        {
            if (l.col[c])
            {
                const uint8_t* p = AttrPtr(l.col[c], s, 2 + c, kColorSize[l.colFmt[c]], nullptr);
                o.col[c] = ReadColor(p, l.colFmt[c]);
            }
            else
                o.col[c] = 0xFFFFFFFF;
        }

        // texture coordinates
        for (int t = 0; t < 8; t++)
        {
            if (l.tex[t])
            {
                uint32_t cs = kCompSize[l.texFmt[t]];
                const uint8_t* p = AttrPtr(l.tex[t], s, 4 + t, cs * (l.texCnt[t] ? 2 : 1), nullptr);
                o.tex[t][0] = ReadComp(p, l.texFmt[t], texScale[t]);
                o.tex[t][1] = l.texCnt[t] ? ReadComp(p + cs, l.texFmt[t], texScale[t]) : 0.0f;
            }
            else
                o.tex[t][0] = o.tex[t][1] = 0;
        }
        o.mtx[0] = mtx[0] | (mtx[1] << 8) | (mtx[2] << 16) | (mtx[3] << 24);
        o.mtx[1] = mtx[4] | (mtx[5] << 8) | (mtx[6] << 16) | (mtx[7] << 24);
        o.mtx[2] = mtx[8];
    }
}
