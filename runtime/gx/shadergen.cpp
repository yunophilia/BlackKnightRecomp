// HLSL generation for the GX transform/lighting unit and the TEV pixel pipeline.
// Semantics follow the hardware as documented by Dolphin's shader generators.
#include "shadergen.h"
#include "vertex_loader.h"

#include <format>

static void W(std::string& s, const std::string& t) { s += t; }

// ---------------------------------------------------------------------- vertex shader
static const char* kVSCommon = R"(
Buffer<float> xf : register(t0);
cbuffer VSConst : register(b0)
{
    row_major float4x4 proj;
    float4 depthp;
    int4 materials[4];
    float4 pixelOffset;
};
float4 xrow(uint r) { uint b = r * 4; return float4(xf[b], xf[b + 1], xf[b + 2], xf[b + 3]); }
float3 nrow(uint b) { return float3(xf[b], xf[b + 1], xf[b + 2]); }
float3 xf3(uint b) { return float3(xf[b], xf[b + 1], xf[b + 2]); }
uint xfu(uint b) { return asuint(xf[b]); }
int4 lightColor(uint i) { uint c = xfu(0x600 + i * 16 + 3); return int4(c >> 24, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF); }
struct VSIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float3 bin : BINORMAL;
    float3 tan : TANGENT;
    float4 col0 : COLOR0;
    float4 col1 : COLOR1;
    float2 tex0 : TEXCOORD0;
    float2 tex1 : TEXCOORD1;
    float2 tex2 : TEXCOORD2;
    float2 tex3 : TEXCOORD3;
    float2 tex4 : TEXCOORD4;
    float2 tex5 : TEXCOORD5;
    float2 tex6 : TEXCOORD6;
    float2 tex7 : TEXCOORD7;
    uint3 mtx : BLENDINDICES;
};
)";

static void GenLight(std::string& s, uint32_t chanReg, int light, bool alpha)
{
    uint32_t diffuse = (chanReg >> 7) & 3;
    uint32_t attn = (chanReg >> 9) & 3;
    std::string swz = alpha ? "a" : "rgb";
    std::string cmp = alpha ? "" : "3";
    std::string b = std::format("0x{:X}", 0x600 + light * 16);
    // attn (XF AttenuationFunc): 0 = none, 1 = specular, 2 = directional, 3 = spot
    if (attn == 1)
    {
        W(s, std::format("\t\tldir = normalize(xf3({0} + 10) - pos.xyz);\n", b));
        W(s, std::format("\t\tattn = (dot(n, ldir) >= 0.0) ? max(0.0, dot(n, xf3({0} + 13))) : 0.0;\n", b));
        W(s, std::format("\t\tcosAttn = xf3({0} + 4);\n", b));
        W(s, std::format("\t\tdistAttn = {1}(xf3({0} + 7));\n", b, diffuse == 0 ? "" : "normalize"));
        W(s, "\t\tattn = max(0.0, dot(cosAttn, float3(1.0, attn, attn * attn))) / dot(distAttn, float3(1.0, attn, attn * attn));\n");
    }
    else if (attn == 3)
    {
        W(s, std::format("\t\tldir = xf3({0} + 10) - pos.xyz;\n", b));
        W(s, "\t\tdist2 = dot(ldir, ldir); dist = sqrt(dist2); ldir = ldir / dist;\n");
        W(s, std::format("\t\tattn = max(0.0, dot(ldir, xf3({0} + 13)));\n", b));
        W(s, std::format("\t\tattn = max(0.0, xf[{0} + 4] + xf[{0} + 5] * attn + xf[{0} + 6] * attn * attn) / dot(xf3({0} + 7), float3(1.0, dist, dist2));\n", b));
    }
    else
    {
        W(s, std::format("\t\tldir = normalize(xf3({0} + 10) - pos.xyz);\n", b));
        W(s, "\t\tattn = 1.0;\n");
        W(s, "\t\tif (length(ldir) == 0.0) ldir = n;\n");
    }
    if (diffuse == 0)
        W(s, std::format("\t\tlacc.{0} += int{1}(round(attn * float{1}(lightColor({2}).{0})));\n", swz, cmp, light));
    else
        W(s, std::format("\t\tlacc.{0} += int{1}(round(attn * {3}dot(ldir, n)) * float{1}(lightColor({2}).{0})));\n", swz, cmp, light,
                         diffuse == 2 ? "max(0.0, " : "("));
}

static void GenLightingChannel(std::string& s, const VSKey& k, int j)
{
    uint32_t c = k.chan[j];
    uint32_t a = k.chan[j + 2];
    W(s, std::format("float4 lighting{}(float4 base, float3 pos, float3 n)\n{{\n", j));
    W(s, "\tint4 lacc; float3 ldir, cosAttn, distAttn; float dist, dist2, attn;\n");
    bool cMatVtx = c & 1, aMatVtx = a & 1;
    W(s, cMatVtx ? "\tint4 mat = int4(round(base * 255.0));\n" : std::format("\tint4 mat = materials[{}];\n", j + 2));
    if (c & 2)
        W(s, (c & 0x40) ? "\tlacc = int4(round(base * 255.0));\n" : std::format("\tlacc = materials[{}];\n", j));
    else
        W(s, "\tlacc = int4(255, 255, 255, 255);\n");
    if (aMatVtx != cMatVtx)
        W(s, aMatVtx ? "\tmat.w = int(round(base.w * 255.0));\n" : std::format("\tmat.w = materials[{}].w;\n", j + 2));
    if (a & 2)
        W(s, (a & 0x40) ? "\tlacc.w = int(round(base.w * 255.0));\n" : std::format("\tlacc.w = materials[{}].w;\n", j));
    else
        W(s, "\tlacc.w = 255;\n");
    auto mask = [](uint32_t r) { return ((r >> 2) & 0xF) | (((r >> 11) & 0xF) << 4); };
    if (c & 2)
        for (int i = 0; i < 8; i++)
            if (mask(c) & (1 << i)) { W(s, "\t{\n"); GenLight(s, c, i, false); W(s, "\t}\n"); }
    if (a & 2)
        for (int i = 0; i < 8; i++)
            if (mask(a) & (1 << i)) { W(s, "\t{\n"); GenLight(s, a, i, true); W(s, "\t}\n"); }
    W(s, "\tlacc = clamp(lacc, 0, 255);\n");
    W(s, "\treturn float4((mat * (lacc + (lacc >> 7))) >> 8) / 255.0;\n}\n");
}

std::string GenerateVertexShader(const VSKey& k)
{
    std::string s = kVSCommon;
    W(s, "struct VSOut\n{\n\tfloat4 pos : SV_Position;\n\tfloat4 col0 : COLOR0;\n\tfloat4 col1 : COLOR1;\n");
    for (uint32_t i = 0; i < k.numTexGens; i++) W(s, std::format("\tfloat3 tex{0} : TEXCOORD{0};\n", i));
    W(s, "\tfloat4 clipPos : TEXCOORD8;\n};\n");
    GenLightingChannel(s, k, 0);
    GenLightingChannel(s, k, 1);

    W(s, "VSOut main(VSIn v)\n{\n\tVSOut o;\n");
    W(s, "\tuint mtx[9] = { v.mtx.x & 0xFF, (v.mtx.x >> 8) & 0xFF, (v.mtx.x >> 16) & 0xFF, v.mtx.x >> 24, v.mtx.y & 0xFF, (v.mtx.y >> 8) & 0xFF, (v.mtx.y >> 16) & 0xFF, v.mtx.y >> 24, v.mtx.z & 0xFF };\n");
    W(s, "\tfloat4 rawpos = float4(v.pos, 1.0);\n");
    W(s, "\tuint pidx = mtx[0];\n");
    W(s, "\tfloat4 pos = float4(dot(xrow(pidx), rawpos), dot(xrow(pidx + 1), rawpos), dot(xrow(pidx + 2), rawpos), 1.0);\n");
    W(s, "\tuint nb = 0x400 + (pidx & 31) * 3;\n");
    bool hasN = k.components & VC_NRM;
    if (hasN)
    {
        W(s, "\tfloat3 n = normalize(float3(dot(nrow(nb), v.nrm), dot(nrow(nb + 3), v.nrm), dot(nrow(nb + 6), v.nrm)));\n");
        W(s, "\tfloat3 _t = float3(dot(nrow(nb), v.tan), dot(nrow(nb + 3), v.tan), dot(nrow(nb + 6), v.tan));\n");
        W(s, "\tfloat3 _b = float3(dot(nrow(nb), v.bin), dot(nrow(nb + 3), v.bin), dot(nrow(nb + 6), v.bin));\n");
    }
    else
        W(s, "\tfloat3 n = float3(0, 0, 1);\n\tfloat3 _t = float3(0, 0, 0);\n\tfloat3 _b = float3(0, 0, 0);\n");

    // vertex colors: color1 only used for channel 1 if both present
    bool c0 = k.components & VC_COL0, c1 = k.components & VC_COL1;
    W(s, std::format("\tfloat4 vc0 = {};\n", c0 ? "v.col0" : (c1 ? "v.col1" : "float4(1,1,1,1)")));
    W(s, std::format("\tfloat4 vc1 = {};\n", (c0 && c1) ? "v.col1" : "float4(1,1,1,1)"));
    W(s, "\to.col0 = lighting0(vc0, pos.xyz, n);\n");
    W(s, "\to.col1 = lighting1(vc1, pos.xyz, n);\n");
    if (k.numColorChans == 0) W(s, "\to.col0 = float4(0,0,0,0);\n");
    if (k.numColorChans < 2) W(s, "\to.col1 = float4(0,0,0,0);\n");

    // texgen
    for (uint32_t i = 0; i < k.numTexGens; i++)
    {
        uint32_t info = k.texMtxInfo[i];
        uint32_t projection = (info >> 1) & 1;
        uint32_t inputForm = (info >> 2) & 1;
        uint32_t type = (info >> 4) & 7;
        uint32_t srcRow = (info >> 7) & 31;
        uint32_t embossSrc = (info >> 12) & 7;
        uint32_t embossLight = (info >> 15) & 7;
        W(s, "\t{\n\t\tfloat4 coord = float4(0.0, 0.0, 1.0, 1.0);\n");
        switch (srcRow)
        {
        case 0: W(s, "\t\tcoord.xyz = rawpos.xyz;\n"); break;
        case 1: if (hasN) W(s, "\t\tcoord.xyz = v.nrm;\n"); break;
        case 2: break;
        case 3: if (k.components & VC_NBT) W(s, "\t\tcoord.xyz = v.tan;\n"); break;
        case 4: if (k.components & VC_NBT) W(s, "\t\tcoord.xyz = v.bin;\n"); break;
        default:
        {
            uint32_t t = srcRow - 5;
            if (t < 8 && (k.components & (VC_TEX0 << t)))
                W(s, std::format("\t\tcoord = float4(v.tex{0}.x, v.tex{0}.y, 1.0, 1.0);\n", t));
            break;
        }
        }
        if (inputForm == 0) W(s, "\t\tcoord.z = 1.0;\n");
        W(s, "\t\tif (isnan(coord.x)) coord.x = 1.0; if (isnan(coord.y)) coord.y = 1.0; if (isnan(coord.z)) coord.z = 1.0;\n");
        W(s, "\t\tfloat3 r;\n");
        if (type == 0)
        {
            W(s, std::format("\t\tuint t = mtx[{}];\n", 1 + i));
            if (projection)
                W(s, "\t\tr = float3(dot(xrow(t), coord), dot(xrow(t + 1), coord), dot(xrow(t + 2), coord));\n");
            else
                W(s, "\t\tr = float3(dot(xrow(t), coord), dot(xrow(t + 1), coord), 1.0);\n");
        }
        else if (type == 1)
        {
            // emboss: shift previous texcoord by the light direction in tangent space
            W(s, std::format("\t\tfloat3 ldir = normalize(xf3(0x{:X} + 10) - pos.xyz);\n", 0x600 + embossLight * 16));
            W(s, std::format("\t\tr = o.tex{} + float3(dot(ldir, _t), dot(ldir, _b), 0.0);\n", embossSrc < i ? embossSrc : 0));
        }
        else if (type == 2)
            W(s, "\t\tr = float3(o.col0.x, o.col0.y, 1.0);\n");
        else
            W(s, "\t\tr = float3(o.col1.x, o.col1.y, 1.0);\n");

        if (k.dualTex && type == 0)
        {
            uint32_t post = k.postMtxInfo[i];
            uint32_t base = (post & 0x3F) * 4;
            if ((post >> 8) & 1) W(s, "\t\tr = normalize(r);\n");
            W(s, std::format("\t\tfloat4 rp = float4(r, 1.0);\n"
                             "\t\tr = float3(dot(float4(xf[0x500+{0}],xf[0x501+{0}],xf[0x502+{0}],xf[0x503+{0}]), rp), "
                             "dot(float4(xf[0x504+{0}],xf[0x505+{0}],xf[0x506+{0}],xf[0x507+{0}]), rp), "
                             "dot(float4(xf[0x508+{0}],xf[0x509+{0}],xf[0x50A+{0}],xf[0x50B+{0}]), rp));\n", base));
        }
        // hardware special case when q is 0 (Dolphin: neheGX lesson 8, Rogue Squadron 3)
        if (type == 0) W(s, "\t\tif (r.z == 0.0) r.xy = clamp(r.xy / 2.0, float2(-1.0, -1.0), float2(1.0, 1.0));\n");
        W(s, std::format("\t\to.tex{} = r;\n\t}}\n", i));
    }

    W(s, "\to.pos = mul(proj, pos);\n");
    W(s, "\to.clipPos = o.pos;\n");
    // GX depth: farZ + zRange * z/w (z/w in [-1, 0]); host depth = gx / 2^24
    W(s, "\to.pos.z = o.pos.z * (1.0 - 1e-7);\n");
    W(s, "\to.pos.z = o.pos.w * depthp.x + o.pos.z * depthp.y;\n");
    W(s, "\to.pos.xy *= depthp.zw;\n");
    W(s, "\to.pos.xy += pixelOffset.xy * o.pos.w;\n");
    W(s, "\treturn o;\n}\n");
    return s;
}

// ---------------------------------------------------------------------- pixel shader
static const char* kColorIn[16] = {
    "prev.rgb", "prev.aaa", "c0.rgb", "c0.aaa", "c1.rgb", "c1.aaa", "c2.rgb", "c2.aaa",
    "textemp.rgb", "textemp.aaa", "rastemp.rgb", "rastemp.aaa", "int3(255,255,255)", "int3(128,128,128)", "konsttemp.rgb", "int3(0,0,0)",
};
static const char* kAlphaIn[8] = { "prev.a", "c0.a", "c1.a", "c2.a", "textemp.a", "rastemp.a", "konsttemp.a", "0" };
static const char* kOut[4] = { "prev", "c0", "c1", "c2" };
static const char* kKC[32] = {
    "int3(255,255,255)", "int3(223,223,223)", "int3(191,191,191)", "int3(159,159,159)", "int3(128,128,128)", "int3(96,96,96)", "int3(64,64,64)", "int3(32,32,32)",
    "int3(0,0,0)", "int3(0,0,0)", "int3(0,0,0)", "int3(0,0,0)",
    "konst[0].rgb", "konst[1].rgb", "konst[2].rgb", "konst[3].rgb",
    "konst[0].rrr", "konst[1].rrr", "konst[2].rrr", "konst[3].rrr",
    "konst[0].ggg", "konst[1].ggg", "konst[2].ggg", "konst[3].ggg",
    "konst[0].bbb", "konst[1].bbb", "konst[2].bbb", "konst[3].bbb",
    "konst[0].aaa", "konst[1].aaa", "konst[2].aaa", "konst[3].aaa",
};
static const char* kKA[32] = {
    "255", "223", "191", "159", "128", "96", "64", "32",
    "0", "0", "0", "0", "0", "0", "0", "0",
    "konst[0].r", "konst[1].r", "konst[2].r", "konst[3].r",
    "konst[0].g", "konst[1].g", "konst[2].g", "konst[3].g",
    "konst[0].b", "konst[1].b", "konst[2].b", "konst[3].b",
    "konst[0].a", "konst[1].a", "konst[2].a", "konst[3].a",
};
static const char* kRas[8] = {
    "int4(round(i.col0 * 255.0))", "int4(round(i.col1 * 255.0))", "int4(0,0,0,0)", "int4(0,0,0,0)", "int4(0,0,0,0)",
    "(int4(1,1,1,1) * alphabump)", "(int4(1,1,1,1) * (alphabump | (alphabump >> 5)))", "int4(0,0,0,0)",
};
static const char* kSwz = "rgba";

static std::string TevRegular(const char* comp, uint32_t bias, uint32_t op, uint32_t scale)
{
    static const char* scaleL[4] = { "", " << 1", " << 2", "" };
    static const char* scaleR[4] = { "", "", "", " >> 1" };
    static const char* lerpBias[2] = { " + 128", " + 127" };
    static const char* biasT[4] = { "", " + 128", " - 128", "" };
    return std::format("(((tevin_d.{0}{1}){2}) {3} (((((tevin_a.{0} << 8) + (tevin_b.{0} - tevin_a.{0}) * (tevin_c.{0} + (tevin_c.{0} >> 7))){2}){4}) >> 8)){5}",
                       comp, biasT[bias], scaleL[scale], op ? '-' : '+', scale != 3 ? lerpBias[op] : "", scaleR[scale]);
}

std::string GeneratePixelShader(const PSKey& k)
{
    std::string s;
    W(s, R"(
cbuffer PSConst : register(b0)
{
    int4 colors[4];
    int4 konst[4];
    int4 alphaRef;
    int4 texDim[8];
    int4 indScale[4];
    int4 indMtx[6];
    int4 zBias[2];
    float4 fogF;
    int4 fogI;
    int4 fogColor;
    float4 fogRange[3];
    float4 depthParams;
};
Texture2D tex[8] : register(t0);
SamplerState samp[8] : register(s0);
int idot(int3 a, int3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
int idot(int4 a, int4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
int4 sampleTex(uint m, int2 uv)
{
    float2 c = float2(uv) / (128.0 * float2(texDim[m].xy));
    float4 r;
    switch (m)
    {
    case 0: r = tex[0].Sample(samp[0], c); break;
    case 1: r = tex[1].Sample(samp[1], c); break;
    case 2: r = tex[2].Sample(samp[2], c); break;
    case 3: r = tex[3].Sample(samp[3], c); break;
    case 4: r = tex[4].Sample(samp[4], c); break;
    case 5: r = tex[5].Sample(samp[5], c); break;
    case 6: r = tex[6].Sample(samp[6], c); break;
    default: r = tex[7].Sample(samp[7], c); break;
    }
    return int4(round(r * 255.0));
}
)");
    W(s, "struct PSIn\n{\n\tfloat4 pos : SV_Position;\n\tfloat4 col0 : COLOR0;\n\tfloat4 col1 : COLOR1;\n");
    for (uint32_t i = 0; i < k.numTexGens; i++) W(s, std::format("\tfloat3 tex{0} : TEXCOORD{0};\n", i));
    W(s, "\tfloat4 clipPos : TEXCOORD8;\n};\n");
    W(s, "struct PSOut\n{\n\tfloat4 col : SV_Target0;\n\tfloat4 col1 : SV_Target1;\n");
    if (k.perPixelDepth) W(s, "\tfloat depth : SV_Depth;\n");
    W(s, "};\n");

    W(s, "PSOut main(PSIn i)\n{\n\tPSOut o;\n");
    W(s, "\tint4 c0 = colors[1], c1 = colors[2], c2 = colors[3], prev = colors[0];\n");
    W(s, "\tint4 rastemp = int4(0,0,0,0), textemp = int4(0,0,0,0), konsttemp = int4(0,0,0,0);\n");
    W(s, "\tint alphabump = 0;\n\tint3 tevcoord = int3(0,0,0);\n\tint2 wrappedcoord = int2(0,0);\n");
    W(s, "\tint4 tevin_a, tevin_b, tevin_c, tevin_d;\n");

    // fixed-point texcoords (7 fractional bits, in texels)
    for (uint32_t t = 0; t < k.numTexGens; t++)
    {
        // which texmap uses this coord: find first stage referencing it for dimensions
        int map = -1;
        for (uint32_t st = 0; st < k.numTevStages && map < 0; st++)
        {
            uint32_t ord = (k.tevOrder[st / 2] >> ((st & 1) * 12)) & 0xFFF;
            if (((ord >> 3) & 7) == t) map = ord & 7;
        }
        for (uint32_t is = 0; is < k.numIndStages && map < 0; is++)
            if (((k.indOrder >> (6 * is + 3)) & 7) == t) map = (k.indOrder >> (6 * is)) & 7;
        if (map < 0) map = 0;
        W(s, std::format("\tfloat2 uvf{0} = (i.tex{0}.z == 0.0) ? i.tex{0}.xy : i.tex{0}.xy / i.tex{0}.z;\n", t));
        W(s, std::format("\tint2 fixpoint_uv{0} = int2(uvf{0} * float2(texDim[{1}].xy) * 128.0);\n", t, map));
    }

    // indirect stages
    for (uint32_t is = 0; is < k.numIndStages; is++)
    {
        uint32_t map = (k.indOrder >> (6 * is)) & 7;
        uint32_t coord = (k.indOrder >> (6 * is + 3)) & 7;
        if (coord < k.numTexGens)
        {
            W(s, std::format("\tint2 indcoord{0} = fixpoint_uv{1} >> indScale[{0}].xy;\n", is, coord));
            W(s, std::format("\tint3 iindtex{0} = sampleTex({1}, indcoord{0}).abg;\n", is, map));
        }
        else
            W(s, std::format("\tint3 iindtex{0} = int3(0,0,0);\n", is));
    }

    for (uint32_t n = 0; n < k.numTevStages; n++)
    {
        uint32_t ord = (k.tevOrder[n / 2] >> ((n & 1) * 12)) & 0xFFF;
        uint32_t texmap = ord & 7, texcoord = (ord >> 3) & 7, texEnable = (ord >> 6) & 1, chan = (ord >> 7) & 7;
        bool hasCoord = texcoord < k.numTexGens;
        if (!hasCoord) texcoord = 0;
        uint32_t ind = k.tevInd[n];
        uint32_t bt = ind & 3, fmt = (ind >> 2) & 3, bias = (ind >> 4) & 7, bs = (ind >> 7) & 3, mid = (ind >> 9) & 15;
        uint32_t sw = (ind >> 13) & 7, tw = (ind >> 16) & 7, fb = (ind >> 20) & 1;
        W(s, std::format("\t// stage {}\n", n));
        bool hasInd = bt < k.numIndStages;
        if (hasInd && bs != 0)
        {
            static const char* sel[4] = { "", "x", "y", "z" };
            static const char shift[4] = { '0', '5', '4', '3' };
            W(s, std::format("\talphabump = (iindtex{}.{} << {}) & 248;\n", bt, sel[bs], shift[fmt]));
        }
        if (hasInd && (mid & 3) != 0 && k.numTexGens > 0)
        {
            static const char fshift[4] = { '0', '3', '4', '5' };
            W(s, std::format("\tint3 iindtevcrd{0} = iindtex{1} >> {2};\n", n, bt, fshift[fmt]));
            static const char* bfield[8] = { "", "x", "y", "xy", "z", "xz", "yz", "xyz" };
            const char* badd = fmt == 0 ? "-128" : "1";
            if (bias)
                W(s, std::format("\tiindtevcrd{}.{} += {};\n", n, bfield[bias], badd));
            uint32_t mtxIdx = 2 * ((mid & 3) - 1);
            uint32_t mtxId = mid >> 2;   // 0 = indirect matrix, 1 = S, 2 = T
            if (mtxId == 0)
                W(s, std::format("\tint2 indtevtrans{0} = int2(idot(indMtx[{1}].xyz, iindtevcrd{0}), idot(indMtx[{2}].xyz, iindtevcrd{0})) >> 3;\n", n, mtxIdx, mtxIdx + 1));
            else if (mtxId == 1)
                W(s, std::format("\tint2 indtevtrans{0} = int2(fixpoint_uv{1} * iindtevcrd{0}.xx) >> 8;\n", n, texcoord));
            else
                W(s, std::format("\tint2 indtevtrans{0} = int2(fixpoint_uv{1} * iindtevcrd{0}.yy) >> 8;\n", n, texcoord));
            W(s, std::format("\tif (indMtx[{1}].w >= 0) indtevtrans{0} >>= indMtx[{1}].w; else indtevtrans{0} <<= (-indMtx[{1}].w);\n", n, mtxIdx));
        }
        else
            W(s, std::format("\tint2 indtevtrans{} = int2(0, 0);\n", n));

        if (k.numTexGens > 0)
        {
            static const char* wrapStart[5] = { "(256<<7)", "(128<<7)", "(64<<7)", "(32<<7)", "(16<<7)" };
            auto wrap = [&](uint32_t w, const char* c) {
                if (w == 0) W(s, std::format("\twrappedcoord.{0} = fixpoint_uv{1}.{0};\n", c, texcoord));
                else if (w >= 6) W(s, std::format("\twrappedcoord.{} = 0;\n", c));
                else W(s, std::format("\twrappedcoord.{0} = fixpoint_uv{1}.{0} & ({2} - 1);\n", c, texcoord, wrapStart[w - 1]));
            };
            wrap(sw, "x");
            wrap(tw, "y");
            W(s, std::format("\ttevcoord.xy {}= wrappedcoord + indtevtrans{};\n", fb ? "+" : "", n));
            W(s, "\ttevcoord.xy = (tevcoord.xy << 8) >> 8;\n");
        }

        uint32_t cc = k.colorComb[n], ac = k.alphaComb[n];
        uint32_t ca = (cc >> 12) & 15, cb = (cc >> 8) & 15, ccc = (cc >> 4) & 15, cd = cc & 15;
        uint32_t aa = (ac >> 13) & 7, ab = (ac >> 10) & 7, acc = (ac >> 7) & 7, ad = (ac >> 4) & 7;
        uint32_t rswap = ac & 3, tswap = (ac >> 2) & 3;
        auto swapStr = [&](uint32_t table) {
            uint32_t rg = k.ksel[table * 2], ba = k.ksel[table * 2 + 1];
            char str[5] = { kSwz[rg & 3], kSwz[(rg >> 2) & 3], kSwz[ba & 3], kSwz[(ba >> 2) & 3], 0 };
            return std::string(str);
        };
        bool usesRas = ca == 10 || ca == 11 || cb == 10 || cb == 11 || ccc == 10 || ccc == 11 || cd == 10 || cd == 11 ||
                       aa == 5 || ab == 5 || acc == 5 || ad == 5;
        if (usesRas)
            W(s, std::format("\trastemp = {}.{};\n", kRas[chan], swapStr(rswap)));
        if (texEnable && k.numTexGens > 0)
            W(s, std::format("\ttextemp = sampleTex({}, tevcoord.xy).{};\n", texmap, swapStr(tswap)));
        else if (k.numTexGens == 0)
            W(s, "\ttextemp = int4(0,0,0,0);\n");
        else
            W(s, "\ttextemp = int4(255,255,255,255);\n");
        bool usesK = ca == 14 || cb == 14 || ccc == 14 || cd == 14 || aa == 6 || ab == 6 || acc == 6 || ad == 6;
        if (usesK)
        {
            uint32_t ks = k.ksel[n / 2];
            uint32_t kc = (n & 1) ? (ks >> 14) & 31 : (ks >> 4) & 31;
            uint32_t ka = (n & 1) ? (ks >> 19) & 31 : (ks >> 9) & 31;
            W(s, std::format("\tkonsttemp = int4({}, {});\n", kKC[kc], kKA[ka]));
        }
        W(s, std::format("\ttevin_a = int4({}, {}) & int4(255,255,255,255);\n", kColorIn[ca], kAlphaIn[aa]));
        W(s, std::format("\ttevin_b = int4({}, {}) & int4(255,255,255,255);\n", kColorIn[cb], kAlphaIn[ab]));
        W(s, std::format("\ttevin_c = int4({}, {}) & int4(255,255,255,255);\n", kColorIn[ccc], kAlphaIn[acc]));
        W(s, std::format("\ttevin_d = int4({}, {});\n", kColorIn[cd], kAlphaIn[ad]));

        uint32_t cbias = (cc >> 16) & 3, cop = (cc >> 18) & 1, cclamp = (cc >> 19) & 1, cscale = (cc >> 20) & 3, cdest = (cc >> 22) & 3;
        uint32_t abias = (ac >> 16) & 3, aop = (ac >> 18) & 1, aclamp = (ac >> 19) & 1, ascale = (ac >> 20) & 3, adest = (ac >> 22) & 3;
        W(s, std::format("\t{}.rgb = clamp(", kOut[cdest]));
        if (cbias != 3)
            W(s, TevRegular("rgb", cbias, cop, cscale));
        else
        {
            static const char* gt[4] = {
                "((tevin_a.r > tevin_b.r) ? tevin_c.rgb : int3(0,0,0))",
                "((idot(tevin_a.rgb, int3(1,256,0)) > idot(tevin_b.rgb, int3(1,256,0))) ? tevin_c.rgb : int3(0,0,0))",
                "((idot(tevin_a.rgb, int3(1,256,65536)) > idot(tevin_b.rgb, int3(1,256,65536))) ? tevin_c.rgb : int3(0,0,0))",
                "(max(sign(tevin_a.rgb - tevin_b.rgb), int3(0,0,0)) * tevin_c.rgb)" };
            static const char* eq[4] = {
                "((tevin_a.r == tevin_b.r) ? tevin_c.rgb : int3(0,0,0))",
                "((idot(tevin_a.rgb, int3(1,256,0)) == idot(tevin_b.rgb, int3(1,256,0))) ? tevin_c.rgb : int3(0,0,0))",
                "((idot(tevin_a.rgb, int3(1,256,65536)) == idot(tevin_b.rgb, int3(1,256,65536))) ? tevin_c.rgb : int3(0,0,0))",
                "((int3(1,1,1) - sign(abs(tevin_a.rgb - tevin_b.rgb))) * tevin_c.rgb)" };
            W(s, std::format("tevin_d.rgb + {}", cop ? eq[cscale] : gt[cscale]));
        }
        W(s, cclamp ? ", int3(0,0,0), int3(255,255,255));\n" : ", int3(-1024,-1024,-1024), int3(1023,1023,1023));\n");
        W(s, std::format("\t{}.a = clamp(", kOut[adest]));
        if (abias != 3)
            W(s, TevRegular("a", abias, aop, ascale));
        else
        {
            static const char* gt[4] = {
                "((tevin_a.r > tevin_b.r) ? tevin_c.a : 0)",
                "((idot(tevin_a.rgb, int3(1,256,0)) > idot(tevin_b.rgb, int3(1,256,0))) ? tevin_c.a : 0)",
                "((idot(tevin_a.rgb, int3(1,256,65536)) > idot(tevin_b.rgb, int3(1,256,65536))) ? tevin_c.a : 0)",
                "((tevin_a.a > tevin_b.a) ? tevin_c.a : 0)" };
            static const char* eq[4] = {
                "((tevin_a.r == tevin_b.r) ? tevin_c.a : 0)",
                "((idot(tevin_a.rgb, int3(1,256,0)) == idot(tevin_b.rgb, int3(1,256,0))) ? tevin_c.a : 0)",
                "((idot(tevin_a.rgb, int3(1,256,65536)) == idot(tevin_b.rgb, int3(1,256,65536))) ? tevin_c.a : 0)",
                "((tevin_a.a == tevin_b.a) ? tevin_c.a : 0)" };
            W(s, std::format("tevin_d.a + {}", aop ? eq[ascale] : gt[ascale]));
        }
        W(s, aclamp ? ", 0, 255);\n" : ", -1024, 1023);\n");
    }
    W(s, "\tprev = prev & 255;\n");

    // alpha test
    {
        uint32_t comp0 = k.alphaFunc & 7, comp1 = (k.alphaFunc >> 3) & 7, logic = (k.alphaFunc >> 6) & 3;
        static const char* f[8] = { "(false)", "(prev.a < {})", "(prev.a == {})", "(prev.a <= {})", "(prev.a > {})", "(prev.a != {})", "(prev.a >= {})", "(true)" };
        static const char* lg[4] = { " && ", " || ", " != ", " == " };
        bool always = (logic == 0 && comp0 == 7 && comp1 == 7) || (logic == 1 && (comp0 == 7 || comp1 == 7));
        if (!always)
        {
            std::string a0 = std::vformat(f[comp0], std::make_format_args("alphaRef.x"));
            std::string a1 = std::vformat(f[comp1], std::make_format_args("alphaRef.y"));
            W(s, std::format("\tif (!({}{}{})) discard;\n", a0, lg[logic], a1));
        }
    }
    W(s, "\tif (prev.a == 1) prev.a = 0;\n");

    // depth
    W(s, "\tint zCoord = int(depthParams.x) + int((i.clipPos.z / i.clipPos.w) * depthParams.y);\n");
    W(s, "\tzCoord = clamp(zCoord, 0, 0xFFFFFF);\n");
    uint32_t zop = (k.zTexType >> 2) & 3;
    if (zop != 0 && k.numTevStages > 0)
    {
        W(s, std::format("\tzCoord = idot(zBias[0], textemp) + zBias[1].w {};\n", zop == 1 ? "+ zCoord" : ""));
        W(s, "\tzCoord = zCoord & 0xFFFFFF;\n");
    }
    if (k.perPixelDepth) W(s, "\to.depth = float(zCoord) / 16777216.0;\n");

    // fog
    uint32_t fsel = k.fogType & 7, fproj = (k.fogType >> 3) & 1, frange = (k.fogType >> 4) & 1;
    if (fsel >= 2)
    {
        if (fproj == 0)
            W(s, "\tfloat ze = (fogF.x * 16777216.0) / float(fogI.y - (zCoord >> fogI.w));\n");
        else
            W(s, "\tfloat ze = fogF.x * float(zCoord) / 16777216.0;\n");
        if (frange)
        {
            W(s, "\tfloat offset = (2.0 * (i.pos.x / fogF.w)) - 1.0 - fogF.z;\n"
                 "\tfloat floatindex = clamp(9.0 - abs(offset) * 9.0, 0.0, 9.0);\n"
                 "\tuint indexlower = uint(floatindex); uint indexupper = indexlower + 1u;\n"
                 "\tfloat klower = fogRange[indexlower >> 2u][indexlower & 3u];\n"
                 "\tfloat kupper = fogRange[indexupper >> 2u][indexupper & 3u];\n"
                 "\tfloat kk = lerp(klower, kupper, frac(floatindex));\n"
                 "\tze *= sqrt(offset * offset + kk * kk) / kk;\n");
        }
        W(s, "\tfloat fog = clamp(ze - fogF.y, 0.0, 1.0);\n");
        switch (fsel)
        {
        case 4: W(s, "\tfog = 1.0 - exp2(-8.0 * fog);\n"); break;
        case 5: W(s, "\tfog = 1.0 - exp2(-8.0 * fog * fog);\n"); break;
        case 6: W(s, "\tfog = exp2(-8.0 * (1.0 - fog));\n"); break;
        case 7: W(s, "\tfog = 1.0 - fog; fog = exp2(-8.0 * fog * fog);\n"); break;
        }
        W(s, "\tint ifog = int(round(fog * 256.0));\n");
        W(s, "\tprev.rgb = (prev.rgb * (256 - ifog) + fogColor.rgb * ifog) >> 8;\n");
    }

    bool rgba6 = k.pixelFormat == 1;
    if (rgba6) W(s, "\to.col.rgb = float3(prev.rgb >> 2) / 63.0;\n");
    else W(s, "\to.col.rgb = float3(prev.rgb) / 255.0;\n");
    if (k.dstAlphaEnable)
        W(s, rgba6 ? "\to.col.a = float(alphaRef.z >> 2) / 63.0;\n" : "\to.col.a = float(alphaRef.z) / 255.0;\n");
    else
        W(s, rgba6 ? "\to.col.a = float(prev.a >> 2) / 63.0;\n" : "\to.col.a = float(prev.a) / 255.0;\n");
    W(s, "\to.col1 = float4(0.0, 0.0, 0.0, float(prev.a) / 255.0);\n");
    W(s, "\treturn o;\n}\n");
    return s;
}
