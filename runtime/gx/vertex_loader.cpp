// GX vertex stream -> HostVertex
//
// Hot path: per draw, attribute formats are fixed, so all per-format decisions are
// hoisted out of the vertex loop into small specialised readers.
#include "vertex_loader.h"
#include "../kernel/memory.h"

#include <cstring>

static inline uint16_t BE16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t BE32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline float BEF(const uint8_t* p) { uint32_t u = BE32(p); float f; memcpy(&f, &u, 4); return f; }

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

typedef void (*CompReader)(const uint8_t* p, float* out, int n, float scale);

template <int FMT>
static void ReadComps(const uint8_t* p, float* out, int n, float scale)
{
    for (int i = 0; i < n; i++)
    {
        if constexpr (FMT == 0) out[i] = (float)p[i] * scale;
        else if constexpr (FMT == 1) out[i] = (float)(int8_t)p[i] * scale;
        else if constexpr (FMT == 2) out[i] = (float)BE16(p + 2 * i) * scale;
        else if constexpr (FMT == 3) out[i] = (float)(int16_t)BE16(p + 2 * i) * scale;
        else out[i] = BEF(p + 4 * i);
    }
}

static CompReader GetReader(uint32_t fmt)
{
    switch (fmt)
    {
    case 0: return ReadComps<0>;
    case 1: return ReadComps<1>;
    case 2: return ReadComps<2>;
    case 3: return ReadComps<3>;
    default: return ReadComps<4>;
    }
}

static inline uint32_t ReadColor(const uint8_t* p, uint32_t fmt)
{
    uint32_t r, g, b, a;
    switch (fmt)
    {
    case 0: { uint16_t v = BE16(p); r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31; r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2); a = 255; break; }
    case 1: case 2: r = p[0]; g = p[1]; b = p[2]; a = 255; break;
    case 3: { uint16_t v = BE16(p); r = ((v >> 12) & 15) * 17; g = ((v >> 8) & 15) * 17; b = ((v >> 4) & 15) * 17; a = (v & 15) * 17; break; }
    case 4: { uint32_t v = ((uint32_t)p[0] << 16) | (p[1] << 8) | p[2]; r = (v >> 18) & 63; g = (v >> 12) & 63; b = (v >> 6) & 63; a = v & 63;
              r = (r << 2) | (r >> 4); g = (g << 2) | (g >> 4); b = (b << 2) | (b >> 4); a = (a << 2) | (a >> 4); break; }
    default: { uint32_t v; memcpy(&v, p, 4); return v; }   // RGBA8 bytes are already in R,G,B,A order
    }
    return r | (g << 8) | (b << 16) | (a << 24);
}

struct Array
{
    const uint8_t* base;
    uint32_t stride;
};

// Fetch attribute data pointer: direct (in stream) or indexed (in array)
static inline const uint8_t* Fetch(uint8_t type, const uint8_t*& s, const Array& arr, uint32_t directSize, bool* nullIndex)
{
    if (type == 1)
    {
        const uint8_t* p = s;
        s += directSize;
        return p;
    }
    uint32_t idx;
    if (type == 2)
    {
        idx = *s++;
        *nullIndex = idx == 0xFF;
    }
    else
    {
        idx = BE16(s);
        s += 2;
        *nullIndex = idx == 0xFFFF;
    }
    return arr.base + idx * arr.stride;
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
    Array arr[12];
    for (int i = 0; i < 12; i++)
    {
        arr[i].base = PhysPtr(g_gx.cp[CP_ARRAY_BASE + i]);
        arr[i].stride = g_gx.cp[CP_ARRAY_STRIDE + i];
    }
    const float posScale = (l.posFmt < 4) ? 1.0f / (float)(1u << l.posShift) : 1.0f;
    const float nrmScale = (l.nrmFmt <= 1) ? 1.0f / 64.0f : (l.nrmFmt < 4 ? 1.0f / 16384.0f : 1.0f);
    const uint32_t posN = l.posCnt ? 3 : 2;
    const uint32_t posSize = kCompSize[l.posFmt] * posN;
    const uint32_t nrmCompSize = kCompSize[l.nrmFmt];
    const CompReader posRead = GetReader(l.posFmt);
    const CompReader nrmRead = GetReader(l.nrmFmt);
    CompReader texRead[8];
    float texScale[8];
    uint32_t texN[8], texSize[8];
    int numTex = 0;
    int texSlot[8];
    for (int t = 0; t < 8; t++)
    {
        if (!l.tex[t]) continue;
        texSlot[numTex++] = t;
        texRead[t] = GetReader(l.texFmt[t]);
        texScale[t] = (l.texFmt[t] < 4) ? 1.0f / (float)(1u << l.texShift[t]) : 1.0f;
        texN[t] = l.texCnt[t] ? 2 : 1;
        texSize[t] = kCompSize[l.texFmt[t]] * texN[t];
    }
    const bool anyTexMtx = l.texMtxIdx[0] | l.texMtxIdx[1] | l.texMtxIdx[2] | l.texMtxIdx[3] | l.texMtxIdx[4] | l.texMtxIdx[5] | l.texMtxIdx[6] | l.texMtxIdx[7];
    const bool posIndexed = l.pos >= 2;
    if (posIndexed) skip.assign(count, 0);
    else skip.clear();
    const uint32_t defMtx0 = defMtx[0] | (defMtx[1] << 8) | (defMtx[2] << 16) | (defMtx[3] << 24);
    const uint32_t defMtx1 = defMtx[4] | (defMtx[5] << 8) | (defMtx[6] << 16) | (defMtx[7] << 24);

    const uint8_t* s = src;
    for (uint32_t v = 0; v < count; v++)
    {
        HostVertex& o = out[v];
        if (l.posMtxIdx || anyTexMtx)
        {
            uint8_t mtx[9];
            memcpy(mtx, defMtx, 9);
            if (l.posMtxIdx) mtx[0] = *s++ & 63;
            for (int i = 0; i < 8; i++)
                if (l.texMtxIdx[i]) mtx[1 + i] = *s++ & 63;
            o.mtx[0] = mtx[0] | (mtx[1] << 8) | (mtx[2] << 16) | (mtx[3] << 24);
            o.mtx[1] = mtx[4] | (mtx[5] << 8) | (mtx[6] << 16) | (mtx[7] << 24);
            o.mtx[2] = mtx[8];
        }
        else
        {
            o.mtx[0] = defMtx0;
            o.mtx[1] = defMtx1;
            o.mtx[2] = defMtx[8];
        }

        bool nullIdx = false;
        if (l.pos)
        {
            const uint8_t* p = Fetch(l.pos, s, arr[0], posSize, &nullIdx);
            posRead(p, o.pos, posN, posScale);
            if (posN == 2) o.pos[2] = 0.0f;
            if (nullIdx && posIndexed) skip[v] = 1;
        }

        if (l.nrm)
        {
            if (l.nrm == 1)
            {
                const uint8_t* p = s;
                s += nrmCompSize * (l.nrmCnt ? 9 : 3);
                nrmRead(p, o.nrm, 3, nrmScale);
                if (l.nrmCnt)
                {
                    nrmRead(p + 3 * nrmCompSize, o.bin, 3, nrmScale);
                    nrmRead(p + 6 * nrmCompSize, o.tan, 3, nrmScale);
                }
            }
            else
            {
                auto idx = [&]() -> uint32_t {
                    if (l.nrm == 2) return *s++;
                    uint32_t x = BE16(s);
                    s += 2;
                    return x;
                };
                if (l.nrmCnt && l.nrmIndex3)
                {
                    uint32_t i0 = idx(), i1 = idx(), i2 = idx();
                    nrmRead(arr[1].base + i0 * arr[1].stride, o.nrm, 3, nrmScale);
                    nrmRead(arr[1].base + i1 * arr[1].stride + 3 * nrmCompSize, o.bin, 3, nrmScale);
                    nrmRead(arr[1].base + i2 * arr[1].stride + 6 * nrmCompSize, o.tan, 3, nrmScale);
                }
                else
                {
                    const uint8_t* p = arr[1].base + idx() * arr[1].stride;
                    nrmRead(p, o.nrm, 3, nrmScale);
                    if (l.nrmCnt)
                    {
                        nrmRead(p + 3 * nrmCompSize, o.bin, 3, nrmScale);
                        nrmRead(p + 6 * nrmCompSize, o.tan, 3, nrmScale);
                    }
                }
            }
        }

        for (int c = 0; c < 2; c++)
        {
            if (l.col[c])
            {
                bool dummy;
                const uint8_t* p = Fetch(l.col[c], s, arr[2 + c], kColorSize[l.colFmt[c]], &dummy);
                o.col[c] = ReadColor(p, l.colFmt[c]);
            }
            else
                o.col[c] = 0xFFFFFFFF;
        }

        for (int k = 0; k < numTex; k++)
        {
            int t = texSlot[k];
            bool dummy;
            const uint8_t* p = Fetch(l.tex[t], s, arr[4 + t], texSize[t], &dummy);
            texRead[t](p, o.tex[t], texN[t], texScale[t]);
            if (texN[t] == 1) o.tex[t][1] = 0.0f;
        }
    }
}
