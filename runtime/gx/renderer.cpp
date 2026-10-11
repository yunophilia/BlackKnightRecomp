// GX -> Direct3D 11 renderer.
#include "gx_state.h"
#include "shadergen.h"
#include "texture_decode.h"
#include "vertex_loader.h"
#include "video.h"
#include "../kernel/memory.h"
#include "../log.h"

#include <d3dcompiler.h>
#include <unordered_map>
#include <vector>

// ------------------------------------------------------------------ constants / helpers
static constexpr int EFB_W = 640, EFB_H = 528;

static uint64_t Hash64(const void* data, size_t n, uint64_t seed = 0x9E3779B97F4A7C15ull)
{
    // simple 64-bit FNV-1a variant over 8-byte words
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = seed ^ (n * 0xff51afd7ed558ccdull);
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h ^= v;
        h *= 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
    {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

template <typename K>
struct KeyHash
{
    size_t operator()(const K& k) const { return (size_t)Hash64(&k, sizeof(K)); }
};

static ID3DBlob* CompileHLSL(const std::string& src, const char* target)
{
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "main", target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL2 | D3DCOMPILE_IEEE_STRICTNESS, 0, &blob, &err);
    if (FAILED(hr))
    {
        LOG_ERROR("HLSL compile error ({}):\n{}\n---- source ----\n{}", target, err ? (const char*)err->GetBufferPointer() : "?", src);
        if (err) err->Release();
        return nullptr;
    }
    if (err) err->Release();
    return blob;
}

static inline float AsFloat(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

// ------------------------------------------------------------------ device objects
static bool s_ready = false;
static ID3D11Texture2D* s_efbColor = nullptr;
static ID3D11RenderTargetView* s_efbRTV = nullptr;
static ID3D11ShaderResourceView* s_efbSRV = nullptr;
static ID3D11Texture2D* s_efbDepth = nullptr;
static ID3D11DepthStencilView* s_efbDSV = nullptr;
static ID3D11ShaderResourceView* s_efbDepthSRV = nullptr;
static ID3D11Texture2D* s_efbColorCopy = nullptr;     // resolve target for sampling EFB during copies
static ID3D11ShaderResourceView* s_efbColorCopySRV = nullptr;
static ID3D11Texture2D* s_efbDepthCopy = nullptr;
static ID3D11ShaderResourceView* s_efbDepthCopySRV = nullptr;
static ID3D11InputLayout* s_layout = nullptr;
static ID3D11Buffer* s_vb = nullptr;
static ID3D11Buffer* s_ib = nullptr;
static ID3D11Buffer* s_vsCB = nullptr;
static ID3D11Buffer* s_psCB = nullptr;
static ID3D11Buffer* s_xfBuf = nullptr;
static ID3D11ShaderResourceView* s_xfSRV = nullptr;
static ID3D11VertexShader* s_quadVS = nullptr;
static ID3D11PixelShader* s_clearPS = nullptr;
static ID3D11PixelShader* s_copyPS = nullptr;
static ID3D11Buffer* s_utilCB = nullptr;
static ID3D11SamplerState* s_pointClamp = nullptr;
static ID3D11SamplerState* s_linearClamp = nullptr;
static ID3D11Texture2D* s_dummyTex = nullptr;
static ID3D11ShaderResourceView* s_dummySRV = nullptr;
static uint32_t s_vbCap = 0, s_ibCap = 0;
static int s_scale = 3;

static std::unordered_map<VSKey, ID3D11VertexShader*, KeyHash<VSKey>> s_vsCache;
static std::unordered_map<PSKey, ID3D11PixelShader*, KeyHash<PSKey>> s_psCache;
static std::unordered_map<uint64_t, ID3D11BlendState*> s_blendCache;
static std::unordered_map<uint64_t, ID3D11DepthStencilState*> s_depthCache;
static std::unordered_map<uint64_t, ID3D11RasterizerState*> s_rasterCache;
static std::unordered_map<uint64_t, ID3D11SamplerState*> s_samplerCache;
static ID3DBlob* s_vsBlobForLayout = nullptr;

// ------------------------------------------------------------------ TMEM (palettes)
static uint8_t s_tmem[1024 * 1024];

// ------------------------------------------------------------------ batch
enum class Topo { Tri, Line, Point };
static std::vector<HostVertex> s_verts;
static std::vector<uint32_t> s_indices;
static Topo s_topo = Topo::Tri;
static VertexLayout s_batchLayout;
static uint32_t s_batchComponents = 0;
static bool s_xfDirty = true;
static std::vector<uint8_t> s_skip;

// ------------------------------------------------------------------ textures
struct TexEntry
{
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    uint32_t w = 0, h = 0;
    uint64_t hash = 0;
    uint32_t lastFrame = 0;
    bool efbCopy = false;
    uint64_t ramHash = 0;    // RAM content hash when an EFB copy was made
    uint32_t hashedFrame = ~0u;   // frame in which `hash` was last verified
    uint32_t ramSize = 0;
};
static std::unordered_map<uint64_t, TexEntry> s_texCache;   // key: addr | fmt | w | h
static std::unordered_map<uint32_t, TexEntry> s_efbCopies;   // key: dest address
static uint32_t s_frame = 0;
static std::vector<uint32_t> s_decodeBuf;

static void ReleaseTex(TexEntry& e)
{
    if (e.srv) e.srv->Release();
    if (e.tex) e.tex->Release();
    e.srv = nullptr;
    e.tex = nullptr;
}

static void CreateTexture(TexEntry& e, uint32_t w, uint32_t h, uint32_t levels, const uint32_t* const* data, bool renderTarget)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = levels;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (renderTarget ? D3D11_BIND_RENDER_TARGET : 0);
    std::vector<D3D11_SUBRESOURCE_DATA> init(levels);
    uint32_t lw = w, lh = h;
    for (uint32_t i = 0; i < levels; i++)
    {
        if (data)
        {
            init[i].pSysMem = data[i];
            init[i].SysMemPitch = lw * 4;
        }
        lw = std::max(1u, lw / 2);
        lh = std::max(1u, lh / 2);
    }
    g_device->CreateTexture2D(&td, data ? init.data() : nullptr, &e.tex);
    g_device->CreateShaderResourceView(e.tex, nullptr, &e.srv);
    e.w = w;
    e.h = h;
}

// ------------------------------------------------------------------ init
static const char* kQuadHLSL = R"(
cbuffer U : register(b0) { float4 rect; float4 uv; float4 color; float4 params; int4 iparams; };
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vsmain(uint id : SV_VertexID)
{
    float2 t = float2(id & 1, id >> 1);
    V o;
    o.pos = float4(lerp(rect.x, rect.z, t.x), lerp(rect.y, rect.w, t.y), params.x, 1);
    o.uv = lerp(uv.xy, uv.zw, t);
    return o;
}
float4 psclear(V i) : SV_Target { return color; }
Texture2D src : register(t0);
Texture2D<float> srcDepth : register(t1);
SamplerState smp : register(s0);
float4 pscopy(V i) : SV_Target
{
    // iparams.x: format, iparams.y: intensity, iparams.z: depth source, iparams.w: half-scale box filter
    float4 c;
    if (iparams.z != 0)
    {
        float d = srcDepth.SampleLevel(smp, i.uv, 0);
        uint z = (uint)(saturate(d) * 16777215.0);
        float h = ((z >> 16) & 0xFF) / 255.0, m = ((z >> 8) & 0xFF) / 255.0, l = (z & 0xFF) / 255.0;
        c = float4(h, m, l, h);
        switch (iparams.x)
        {
        case 0: case 1: case 8: c = float4(h, h, h, h); break;          // Z4 / Z8 (high)
        case 9: c = float4(m, m, m, m); break;                           // Z8M
        case 10: c = float4(l, l, l, l); break;                          // Z8L
        case 3: c = float4(m, m, m, h); break;                           // Z16 (RA8 layout)
        case 11: case 12: c = float4(m, l, l, m); break;                 // Z16L
        default: c = float4(h, m, l, 1.0); break;                        // Z24X8
        }
        return c;
    }
    c = src.Sample(smp, i.uv);
    if (iparams.w != 0)
    {
        float2 d = params.zw * 0.25;
        c = (src.Sample(smp, i.uv + float2(-d.x, -d.y)) + src.Sample(smp, i.uv + float2(d.x, -d.y)) +
             src.Sample(smp, i.uv + float2(-d.x, d.y)) + src.Sample(smp, i.uv + float2(d.x, d.y))) * 0.25;
    }
    if (iparams.y != 0)
    {
        // RGB -> Y (BT.601, studio range as on hardware)
        float y = dot(c.rgb, float3(0.257, 0.504, 0.098)) + 16.0 / 255.0;
        c.rgb = float3(y, y, y);
    }
    switch (iparams.x)
    {
    case 0: case 1: c = iparams.y ? float4(c.r, c.r, c.r, c.r) : float4(c.r, c.r, c.r, c.r); break;   // R4/R8 (I4/I8)
    case 2: case 3: c = float4(c.r, c.r, c.r, c.a); break;   // RA4 / RA8 (IA4/IA8)
    case 4: c.a = 1.0; break;                                 // RGB565
    case 7: c = float4(c.a, c.a, c.a, c.a); break;           // A8
    case 8: c = float4(c.r, c.r, c.r, c.r); break;           // R8
    case 9: c = float4(c.g, c.g, c.g, c.g); break;           // G8
    case 10: c = float4(c.b, c.b, c.b, c.b); break;          // B8
    case 11: c = float4(c.r, c.r, c.r, c.g); break;          // RG8
    case 12: c = float4(c.g, c.g, c.g, c.b); break;          // GB8
    default: break;
    }
    return c;
}
)";

static bool InitDevice()
{
    s_scale = g_videoConfig.internalScale;
    // EFB color
    D3D11_TEXTURE2D_DESC td{};
    td.Width = EFB_W * s_scale;
    td.Height = EFB_H * s_scale;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    g_device->CreateTexture2D(&td, nullptr, &s_efbColor);
    g_device->CreateRenderTargetView(s_efbColor, nullptr, &s_efbRTV);
    g_device->CreateShaderResourceView(s_efbColor, nullptr, &s_efbSRV);
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    g_device->CreateTexture2D(&td, nullptr, &s_efbColorCopy);
    g_device->CreateShaderResourceView(s_efbColorCopy, nullptr, &s_efbColorCopySRV);
    // EFB depth
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    g_device->CreateTexture2D(&td, nullptr, &s_efbDepth);
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    g_device->CreateDepthStencilView(s_efbDepth, &dsv, &s_efbDSV);
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_R32_FLOAT;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(s_efbDepth, &srvd, &s_efbDepthSRV);
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    g_device->CreateTexture2D(&td, nullptr, &s_efbDepthCopy);
    g_device->CreateShaderResourceView(s_efbDepthCopy, &srvd, &s_efbDepthCopySRV);

    // input layout from a representative vertex shader
    VSKey k{};
    k.components = VC_NRM | VC_COL0 | VC_COL1;
    std::string vsrc = GenerateVertexShader(k);
    s_vsBlobForLayout = CompileHLSL(vsrc, "vs_5_0");
    if (!s_vsBlobForLayout) return false;
    D3D11_INPUT_ELEMENT_DESC ie[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(HostVertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(HostVertex, nrm), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(HostVertex, bin), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(HostVertex, tan), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(HostVertex, col[0]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(HostVertex, col[1]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(HostVertex, tex[0]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(HostVertex, tex[1]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(HostVertex, tex[2]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(HostVertex, tex[3]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 4, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(HostVertex, tex[4]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 5, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(HostVertex, tex[5]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 6, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(HostVertex, tex[6]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 7, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(HostVertex, tex[7]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "BLENDINDICES", 0, DXGI_FORMAT_R32G32B32_UINT, 0, offsetof(HostVertex, mtx), D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(g_device->CreateInputLayout(ie, _countof(ie), s_vsBlobForLayout->GetBufferPointer(), s_vsBlobForLayout->GetBufferSize(), &s_layout)))
    {
        LOG_ERROR("CreateInputLayout failed");
        return false;
    }

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = (sizeof(VSConstants) + 15) & ~15u;
    g_device->CreateBuffer(&bd, nullptr, &s_vsCB);
    bd.ByteWidth = (sizeof(PSConstants) + 15) & ~15u;
    g_device->CreateBuffer(&bd, nullptr, &s_psCB);
    bd.ByteWidth = 80;
    g_device->CreateBuffer(&bd, nullptr, &s_utilCB);

    // XF memory buffer (0x680 floats)
    D3D11_BUFFER_DESC xb{};
    xb.ByteWidth = 0x680 * 4;
    xb.Usage = D3D11_USAGE_DEFAULT;
    xb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    g_device->CreateBuffer(&xb, nullptr, &s_xfBuf);
    D3D11_SHADER_RESOURCE_VIEW_DESC xs{};
    xs.Format = DXGI_FORMAT_R32_FLOAT;
    xs.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    xs.Buffer.NumElements = 0x680;
    g_device->CreateShaderResourceView(s_xfBuf, &xs, &s_xfSRV);

    auto cvs = [&](const char* entry, const char* target) -> ID3DBlob* {
        ID3DBlob* b = nullptr; ID3DBlob* e = nullptr;
        if (FAILED(D3DCompile(kQuadHLSL, strlen(kQuadHLSL), nullptr, nullptr, nullptr, entry, target, 0, 0, &b, &e)))
        {
            LOG_ERROR("util shader: {}", e ? (const char*)e->GetBufferPointer() : "?");
            return nullptr;
        }
        return b;
    };
    ID3DBlob* b = cvs("vsmain", "vs_5_0");
    g_device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s_quadVS);
    b->Release();
    b = cvs("psclear", "ps_5_0");
    g_device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s_clearPS);
    b->Release();
    b = cvs("pscopy", "ps_5_0");
    g_device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s_copyPS);
    b->Release();

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_device->CreateSamplerState(&sd, &s_pointClamp);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    g_device->CreateSamplerState(&sd, &s_linearClamp);

    uint32_t white = 0xFFFFFFFF;
    D3D11_TEXTURE2D_DESC dt{};
    dt.Width = dt.Height = 1;
    dt.MipLevels = dt.ArraySize = 1;
    dt.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dt.SampleDesc.Count = 1;
    dt.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA di{ &white, 4, 4 };
    g_device->CreateTexture2D(&dt, &di, &s_dummyTex);
    g_device->CreateShaderResourceView(s_dummyTex, nullptr, &s_dummySRV);

    float clear[4] = { 0, 0, 0, 0 };
    g_ctx->ClearRenderTargetView(s_efbRTV, clear);
    g_ctx->ClearDepthStencilView(s_efbDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);
    return true;
}

void Renderer_Init()
{
    if (!Video_Init()) FATAL("video init failed");
    if (!InitDevice()) FATAL("renderer init failed");
    s_ready = true;
}

// ------------------------------------------------------------------ state -> keys
static uint32_t BP(uint32_t r) { return g_gx.bp[r]; }

static void BuildVSKey(VSKey& k)
{
    memset(&k, 0, sizeof(k));
    k.components = s_batchComponents;
    k.numTexGens = BP(0x00) & 15;
    k.numColorChans = (BP(0x00) >> 4) & 7;
    k.dualTex = g_gx.xfregs[XF_DUALTEX] & 1;
    for (uint32_t i = 0; i < k.numTexGens && i < 8; i++)
    {
        k.texMtxInfo[i] = g_gx.xfregs[XF_TEXMTXINFO + i] & 0x3FFFE;
        if (k.dualTex) k.postMtxInfo[i] = g_gx.xfregs[XF_POSTMTXINFO + i] & 0x13F;
    }
    for (int c = 0; c < 4; c++) k.chan[c] = g_gx.xfregs[XF_COLOR0CNTRL + (c >> 1) + (c & 1) * 2] & 0x7FFF;
    // order: color0, color1, alpha0, alpha1
    k.chan[0] = g_gx.xfregs[XF_COLOR0CNTRL] & 0x7FFF;
    k.chan[1] = g_gx.xfregs[XF_COLOR1CNTRL] & 0x7FFF;
    k.chan[2] = g_gx.xfregs[XF_ALPHA0CNTRL] & 0x7FFF;
    k.chan[3] = g_gx.xfregs[XF_ALPHA1CNTRL] & 0x7FFF;
    // lighting details only matter when lighting is enabled
    for (int c = 0; c < 4; c++)
        if (!(k.chan[c] & 2)) k.chan[c] &= 0x41;
}

static uint32_t PixelFormat() { return BP(0x43) & 7; }

static void BuildPSKey(PSKey& k)
{
    memset(&k, 0, sizeof(k));
    uint32_t gen = BP(0x00);
    k.numTexGens = gen & 15;
    k.numTevStages = ((gen >> 10) & 15) + 1;
    k.numIndStages = (gen >> 16) & 7;
    for (uint32_t i = 0; i < k.numTevStages; i++)
    {
        k.colorComb[i] = BP(0xC0 + 2 * i) & 0xFFFFFF;
        k.alphaComb[i] = BP(0xC1 + 2 * i) & 0xFFFFFF;
        k.tevInd[i] = BP(0x10 + i) & 0x1FFFFF;
    }
    for (uint32_t i = 0; i < 8; i++)
    {
        k.tevOrder[i] = (i * 2 < k.numTevStages) ? BP(0x28 + i) & 0xFFFFFF : 0;
        k.ksel[i] = BP(0xF6 + i) & 0xFFFFFF;
    }
    k.indOrder = k.numIndStages ? BP(0x27) & 0xFFFFFF : 0;
    uint32_t af = BP(0xF3);
    k.alphaFunc = ((af >> 16) & 7) | (((af >> 19) & 7) << 3) | (((af >> 22) & 3) << 6);
    k.zTexType = (BP(0xF5) & 3) | (((BP(0xF5) >> 2) & 3) << 2);
    uint32_t fogc = BP(0xF1);
    uint32_t fsel = (fogc >> 21) & 7;
    k.fogType = fsel | (((fogc >> 20) & 1) << 3) | (((BP(0xE8) >> 10) & 1) << 4);
    if (fsel < 2) k.fogType = 0;
    k.pixelFormat = PixelFormat();
    k.dstAlphaEnable = (BP(0x42) >> 8) & 1;
    bool zEnable = BP(0x40) & 1;
    k.perPixelDepth = ((k.zTexType >> 2) & 3) != 0 && zEnable ? 1 : 0;
}

static ID3D11VertexShader* GetVS(const VSKey& k)
{
    auto it = s_vsCache.find(k);
    if (it != s_vsCache.end()) return it->second;
    ID3D11VertexShader* vs = nullptr;
    ID3DBlob* b = CompileHLSL(GenerateVertexShader(k), "vs_5_0");
    if (b)
    {
        g_device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs);
        b->Release();
    }
    s_vsCache[k] = vs;
    return vs;
}

static ID3D11PixelShader* GetPS(const PSKey& k)
{
    auto it = s_psCache.find(k);
    if (it != s_psCache.end()) return it->second;
    ID3D11PixelShader* ps = nullptr;
    std::string src = GeneratePixelShader(k);
    {
        static int n = 0;
        char name[64];
        snprintf(name, sizeof(name), "ps_%03d.hlsl", n++);
        if (n < 30) { FILE* f = fopen(name, "w"); if (f) { fputs(src.c_str(), f); fclose(f); } }
    }
    ID3DBlob* b = CompileHLSL(src, "ps_5_0");
    if (b)
    {
        g_device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps);
        b->Release();
    }
    s_psCache[k] = ps;
    return ps;
}

// ------------------------------------------------------------------ fixed function state
static ID3D11BlendState* GetBlendState()
{
    uint32_t bm = BP(0x41);
    {
        static std::unordered_map<uint32_t, int> seen;
        if (seen.size() < 20 && !seen.count(bm)) { seen[bm] = 1; LOG_INFO("blend mode {:06x} zmode {:02x} pe {:x}", bm, BP(0x40), BP(0x43)); }
    }
    uint32_t fmt = PixelFormat();
    bool hasAlpha = fmt == 1;   // RGBA6_Z24
    uint64_t key = (uint64_t)(bm & 0xFFFF) | ((uint64_t)hasAlpha << 16);
    auto it = s_blendCache.find(key);
    if (it != s_blendCache.end()) return it->second;

    D3D11_BLEND_DESC d{};
    auto& rt = d.RenderTarget[0];
    bool enable = bm & 1;
    bool logic = (bm >> 1) & 1;
    uint32_t dstF = (bm >> 5) & 7, srcF = (bm >> 8) & 7;
    bool sub = (bm >> 11) & 1;
    uint32_t logicMode = (bm >> 12) & 15;
    rt.RenderTargetWriteMask = 0;
    if ((bm >> 3) & 1) rt.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (((bm >> 4) & 1) && hasAlpha) rt.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    auto srcColor = [&](uint32_t f) -> D3D11_BLEND {
        switch (f)
        {
        case 0: return D3D11_BLEND_ZERO;
        case 1: return D3D11_BLEND_ONE;
        case 2: return D3D11_BLEND_DEST_COLOR;
        case 3: return D3D11_BLEND_INV_DEST_COLOR;
        case 4: return D3D11_BLEND_SRC1_ALPHA;
        case 5: return D3D11_BLEND_INV_SRC1_ALPHA;
        case 6: return hasAlpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_ONE;
        default: return hasAlpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_ZERO;
        }
    };
    auto dstColor = [&](uint32_t f) -> D3D11_BLEND {
        switch (f)
        {
        case 0: return D3D11_BLEND_ZERO;
        case 1: return D3D11_BLEND_ONE;
        case 2: return D3D11_BLEND_SRC_COLOR;
        case 3: return D3D11_BLEND_INV_SRC_COLOR;
        case 4: return D3D11_BLEND_SRC1_ALPHA;
        case 5: return D3D11_BLEND_INV_SRC1_ALPHA;
        case 6: return hasAlpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_ONE;
        default: return hasAlpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_ZERO;
        }
    };
    auto toAlpha = [](D3D11_BLEND b) -> D3D11_BLEND {
        switch (b)
        {
        case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
        case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
        case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
        case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
        default: return b;
        }
    };
    if (enable)
    {
        rt.BlendEnable = TRUE;
        if (sub)
        {
            rt.SrcBlend = rt.DestBlend = D3D11_BLEND_ONE;
            rt.BlendOp = D3D11_BLEND_OP_REV_SUBTRACT;
        }
        else
        {
            rt.SrcBlend = srcColor(srcF);
            rt.DestBlend = dstColor(dstF);
            rt.BlendOp = D3D11_BLEND_OP_ADD;
        }
        rt.SrcBlendAlpha = toAlpha(rt.SrcBlend);
        rt.DestBlendAlpha = toAlpha(rt.DestBlend);
        rt.BlendOpAlpha = rt.BlendOp;
    }
    else if (logic)
    {
        // approximate the common logic ops with blending
        switch (logicMode)
        {
        case 0:   // clear
            rt.BlendEnable = TRUE; rt.SrcBlend = rt.DestBlend = D3D11_BLEND_ZERO; rt.BlendOp = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = rt.DestBlendAlpha = D3D11_BLEND_ZERO; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            break;
        case 5:   // noop
            rt.RenderTargetWriteMask = 0;
            break;
        case 15:  // set
            rt.BlendEnable = TRUE; rt.SrcBlend = rt.DestBlend = D3D11_BLEND_ONE; rt.BlendOp = D3D11_BLEND_OP_MAX;
            rt.SrcBlendAlpha = rt.DestBlendAlpha = D3D11_BLEND_ONE; rt.BlendOpAlpha = D3D11_BLEND_OP_MAX;
            break;
        case 10: case 12:   // invert / copy inverted (approximate with 1 - dst)
            rt.BlendEnable = TRUE; rt.SrcBlend = D3D11_BLEND_INV_DEST_COLOR; rt.DestBlend = D3D11_BLEND_ZERO; rt.BlendOp = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D11_BLEND_INV_DEST_ALPHA; rt.DestBlendAlpha = D3D11_BLEND_ZERO; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            break;
        default:  // copy and others
            break;
        }
    }
    ID3D11BlendState* s = nullptr;
    g_device->CreateBlendState(&d, &s);
    s_blendCache[key] = s;
    return s;
}

int g_dbgNoDepth = 0, g_dbgNoCull = 0;
static ID3D11DepthStencilState* GetDepthState()
{
    uint32_t zm = BP(0x40) & 0x1F;
    if (g_dbgNoDepth) zm &= ~1u;
    auto it = s_depthCache.find(zm);
    if (it != s_depthCache.end()) return it->second;
    D3D11_DEPTH_STENCIL_DESC d{};
    static const D3D11_COMPARISON_FUNC funcs[8] = {
        D3D11_COMPARISON_NEVER, D3D11_COMPARISON_LESS, D3D11_COMPARISON_EQUAL, D3D11_COMPARISON_LESS_EQUAL,
        D3D11_COMPARISON_GREATER, D3D11_COMPARISON_NOT_EQUAL, D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_ALWAYS,
    };
    if (zm & 1)
    {
        d.DepthEnable = TRUE;
        d.DepthFunc = funcs[(zm >> 1) & 7];
        d.DepthWriteMask = (zm & 0x10) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    }
    else
    {
        d.DepthEnable = FALSE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = D3D11_COMPARISON_ALWAYS;
    }
    ID3D11DepthStencilState* s = nullptr;
    g_device->CreateDepthStencilState(&d, &s);
    s_depthCache[zm] = s;
    return s;
}

static ID3D11RasterizerState* GetRasterState(uint32_t cull)
{
    auto it = s_rasterCache.find(cull);
    if (it != s_rasterCache.end()) return it->second;
    D3D11_RASTERIZER_DESC d{};
    d.FillMode = D3D11_FILL_SOLID;
    // GX: 1 = cull back (clockwise front), 2 = cull front
    d.CullMode = cull == 1 ? D3D11_CULL_BACK : cull == 2 ? D3D11_CULL_FRONT : D3D11_CULL_NONE;
    d.FrontCounterClockwise = FALSE;   // GX front faces are clockwise in screen space
    d.ScissorEnable = TRUE;
    d.DepthClipEnable = FALSE;
    ID3D11RasterizerState* s = nullptr;
    g_device->CreateRasterizerState(&d, &s);
    s_rasterCache[cull] = s;
    return s;
}

static ID3D11SamplerState* GetSampler(uint32_t mode0, uint32_t mode1, bool hasMips)
{
    uint64_t key = (uint64_t)mode0 | ((uint64_t)mode1 << 32);
    if (!hasMips) key |= 1ull << 63;
    auto it = s_samplerCache.find(key);
    if (it != s_samplerCache.end()) return it->second;
    D3D11_SAMPLER_DESC d{};
    static const D3D11_TEXTURE_ADDRESS_MODE wrap[4] = { D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_MIRROR, D3D11_TEXTURE_ADDRESS_WRAP };
    d.AddressU = wrap[mode0 & 3];
    d.AddressV = wrap[(mode0 >> 2) & 3];
    d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    bool magLinear = (mode0 >> 4) & 1;
    uint32_t minF = (mode0 >> 5) & 7;
    bool minLinear = minF >= 4;
    bool mipLinear = (minF & 3) == 2;
    bool useMips = hasMips && (minF & 3) != 0;
    int f = (minLinear ? 0x10 : 0) | (magLinear ? 0x4 : 0) | (mipLinear ? 0x1 : 0);
    d.Filter = (D3D11_FILTER)f;
    uint32_t aniso = (mode0 >> 19) & 3;
    if (aniso && minLinear && magLinear)
    {
        d.Filter = D3D11_FILTER_ANISOTROPIC;
        d.MaxAnisotropy = 1u << aniso;
    }
    d.MipLODBias = (float)(int8_t)((mode0 >> 9) & 0xFF) / 32.0f;
    d.MinLOD = useMips ? (float)(mode1 & 0xFF) / 16.0f : 0.0f;
    d.MaxLOD = useMips ? (float)((mode1 >> 8) & 0xFF) / 16.0f : 0.0f;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ID3D11SamplerState* s = nullptr;
    g_device->CreateSamplerState(&d, &s);
    s_samplerCache[key] = s;
    return s;
}

// ------------------------------------------------------------------ texture lookup
static uint32_t TexReg(int map, uint32_t base03, uint32_t base47)
{
    return map < 4 ? BP(base03 + map) : BP(base47 + (map - 4));
}

static bool Capturing();
static ID3D11ShaderResourceView* GetTexture(int map, uint32_t* outW, uint32_t* outH, bool* hasMips)
{
    uint32_t img0 = TexReg(map, 0x88, 0xA8);
    uint32_t img3 = TexReg(map, 0x94, 0xB4);
    uint32_t mode1 = TexReg(map, 0x84, 0xA4);
    uint32_t tlutReg = TexReg(map, 0x98, 0xB8);
    uint32_t w = (img0 & 0x3FF) + 1;
    uint32_t h = ((img0 >> 10) & 0x3FF) + 1;
    uint32_t fmt = (img0 >> 20) & 15;
    uint32_t addr = (img3 & 0x00FFFFFF) << 5;
    *outW = w;
    *outH = h;
    *hasMips = false;

    // EFB copy at this address?
    auto ec = s_efbCopies.find(addr);
    if (ec != s_efbCopies.end())
    {
        TexEntry& e = ec->second;
        if (e.hashedFrame == s_frame || e.ramHash == Hash64(PhysPtr(addr), e.ramSize))
        {
            e.lastFrame = s_frame;
            e.hashedFrame = s_frame;
            return e.srv;
        }
        ReleaseTex(e);
        s_efbCopies.erase(ec);
    }

    uint32_t maxLod = ((mode1 >> 8) & 0xFF) >> 4;
    uint32_t levels = 1;
    uint32_t mode0 = TexReg(map, 0x80, 0xA0);
    if (((mode0 >> 5) & 3) != 0 && maxLod > 0)
    {
        uint32_t lw = w, lh = h;
        while (levels <= maxLod && (lw > 1 || lh > 1))
        {
            lw = std::max(1u, lw / 2);
            lh = std::max(1u, lh / 2);
            levels++;
        }
    }
    // total encoded size
    uint32_t total = 0;
    {
        uint32_t lw = w, lh = h;
        for (uint32_t i = 0; i < levels; i++)
        {
            total += GXTex_Size(fmt, lw, lh);
            lw = std::max(1u, lw / 2);
            lh = std::max(1u, lh / 2);
        }
    }
    const uint8_t* src = PhysPtr(addr);
    bool isCI = fmt == TF_C4 || fmt == TF_C8 || fmt == TF_C14X2;
    uint64_t key0 = (uint64_t)addr | ((uint64_t)fmt << 32) | ((uint64_t)w << 36) | ((uint64_t)h << 47) | ((uint64_t)(levels > 1) << 58) | ((uint64_t)isCI << 59);
    if (!isCI && !Capturing())
    {
        auto fast = s_texCache.find(key0);
        if (fast != s_texCache.end() && fast->second.srv && fast->second.hashedFrame == s_frame)
        {
            *hasMips = levels > 1;
            return fast->second.srv;
        }
    }
    uint32_t tlutOff = (tlutReg & 0x3FF) << 9;
    uint32_t tlutFmt = (tlutReg >> 10) & 3;
    uint32_t palSize = fmt == TF_C4 ? 32 : fmt == TF_C8 ? 512 : 0x8000;
    const uint8_t* pal = s_tmem + (tlutOff & (sizeof(s_tmem) - 1));
    uint64_t hash = Hash64(src, total);
    if (isCI) hash = Hash64(pal, std::min<uint32_t>(palSize, (uint32_t)sizeof(s_tmem) - (tlutOff & (sizeof(s_tmem) - 1))), hash) ^ tlutFmt;
    uint64_t key = (uint64_t)addr | ((uint64_t)fmt << 32) | ((uint64_t)w << 36) | ((uint64_t)h << 47) | ((uint64_t)(levels > 1) << 58) | ((uint64_t)isCI << 59);

    TexEntry& e = s_texCache[key];
    *hasMips = levels > 1;
    if (e.srv && e.hash == hash && !Capturing())
    {
        e.lastFrame = s_frame;
        e.hashedFrame = s_frame;
        return e.srv;
    }
    ReleaseTex(e);
    std::vector<std::vector<uint32_t>> lv(levels);
    std::vector<const uint32_t*> ptrs(levels);
    uint32_t lw = w, lh = h, off = 0;
    for (uint32_t i = 0; i < levels; i++)
    {
        lv[i].resize(lw * lh);
        GXTex_Decode(lv[i].data(), src + off, lw, lh, fmt, pal, tlutFmt);
        ptrs[i] = lv[i].data();
        off += GXTex_Size(fmt, lw, lh);
        lw = std::max(1u, lw / 2);
        lh = std::max(1u, lh / 2);
    }
    CreateTexture(e, w, h, levels, ptrs.data(), false);
    if (Capturing())
    {
        char path[64];
        snprintf(path, sizeof(path), "tex_%08x_f%u.bmp", addr, fmt);
        FILE* f = fopen(path, "wb");
        if (f)
        {
            uint32_t rowBytes = w * 4, img = rowBytes * h;
            uint8_t hdr[54] = { 'B', 'M' };
            auto put32 = [&](int o, uint32_t v) { memcpy(hdr + o, &v, 4); };
            put32(2, 54 + img); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
            hdr[26] = 1; hdr[28] = 32; put32(34, img);
            fwrite(hdr, 1, 54, f);
            for (int y = (int)h - 1; y >= 0; y--)
                for (uint32_t x = 0; x < w; x++)
                {
                    uint32_t c = lv[0][y * w + x];
                    uint8_t px[4] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c, (uint8_t)(c >> 24) };
                    fwrite(px, 1, 4, f);
                }
            fclose(f);
        }
    }
    {
        static int n = 0;
        if (n++ < 40) LOG_INFO("tex map{} addr={:08x} {}x{} fmt={} levels={} tlutfmt={}", map, addr, w, h, fmt, levels, tlutFmt);
    }
    e.hash = hash;
    e.lastFrame = s_frame;
    e.hashedFrame = s_frame;
    return e.srv;
}

// ------------------------------------------------------------------ constants
static void FillVSConstants(VSConstants& c, float* flipX, float* flipY)
{
    memset(&c, 0, sizeof(c));
    const uint32_t* p = &g_gx.xfregs[XF_PROJECTION];
    float p0 = AsFloat(p[0]), p1 = AsFloat(p[1]), p2 = AsFloat(p[2]), p3 = AsFloat(p[3]), p4 = AsFloat(p[4]), p5 = AsFloat(p[5]);
    if (p[6] == 0)   // perspective
    {
        float m[4][4] = { { p0, 0, p1, 0 }, { 0, p2, p3, 0 }, { 0, 0, p4, p5 }, { 0, 0, -1, 0 } };
        memcpy(c.proj, m, sizeof(m));
    }
    else
    {
        float m[4][4] = { { p0, 0, 0, p1 }, { 0, p2, 0, p3 }, { 0, 0, p4, p5 }, { 0, 0, 0, 1 } };
        memcpy(c.proj, m, sizeof(m));
    }
    float zRange = XFFloat(0x1000 + XF_VIEWPORT + 2);
    float farZ = XFFloat(0x1000 + XF_VIEWPORT + 5);
    c.depth[0] = farZ / 16777216.0f;
    c.depth[1] = zRange / 16777216.0f;
    c.depth[2] = *flipX;
    c.depth[3] = *flipY;
    auto col = [](uint32_t v, int32_t* o) { o[0] = v >> 24; o[1] = (v >> 16) & 0xFF; o[2] = (v >> 8) & 0xFF; o[3] = v & 0xFF; };
    col(g_gx.xfregs[XF_AMBIENT0], c.materials[0]);
    col(g_gx.xfregs[XF_AMBIENT1], c.materials[1]);
    col(g_gx.xfregs[XF_MATERIAL0], c.materials[2]);
    col(g_gx.xfregs[XF_MATERIAL1], c.materials[3]);
}

static void FillPSConstants(PSConstants& c, const uint32_t* texW, const uint32_t* texH)
{
    memset(&c, 0, sizeof(c));
    for (int i = 0; i < 4; i++)
    {
        uint32_t lo = BP(0xE0 + 2 * i), hi = BP(0xE1 + 2 * i);
        auto s11 = [](uint32_t v) { return (int32_t)(v << 21) >> 21; };
        c.colors[i][0] = s11(lo & 0x7FF);
        c.colors[i][3] = s11((lo >> 12) & 0x7FF);
        c.colors[i][2] = s11(hi & 0x7FF);
        c.colors[i][1] = s11((hi >> 12) & 0x7FF);
    }
    extern int32_t g_tevKonst[4][4];
    memcpy(c.konst, g_tevKonst, sizeof(c.konst));
    uint32_t af = BP(0xF3);
    c.alphaRef[0] = af & 0xFF;
    c.alphaRef[1] = (af >> 8) & 0xFF;
    c.alphaRef[2] = BP(0x42) & 0xFF;
    for (int i = 0; i < 8; i++)
    {
        c.texDim[i][0] = texW[i];
        c.texDim[i][1] = texH[i];
    }
    for (int i = 0; i < 4; i++)
    {
        uint32_t ss = BP(0x25 + (i >> 1));
        uint32_t v = (i & 1) ? (ss >> 8) : ss;
        c.indTexScale[i][0] = v & 15;
        c.indTexScale[i][1] = (v >> 4) & 15;
    }
    for (int m = 0; m < 3; m++)
    {
        uint32_t ra = BP(0x06 + 3 * m), rb = BP(0x07 + 3 * m), rc = BP(0x08 + 3 * m);
        auto s11 = [](uint32_t v) { return (int32_t)(v << 21) >> 21; };
        int32_t ma = s11(ra & 0x7FF), mb = s11((ra >> 11) & 0x7FF);
        int32_t mc = s11(rb & 0x7FF), md = s11((rb >> 11) & 0x7FF);
        int32_t me = s11(rc & 0x7FF), mf = s11((rc >> 11) & 0x7FF);
        int32_t scale = (int32_t)(((ra >> 22) & 3) | (((rb >> 22) & 3) << 2) | (((rc >> 22) & 3) << 4)) - 17;
        // rows: [ma mc me], [mb md mf]
        c.indTexMtx[2 * m][0] = ma; c.indTexMtx[2 * m][1] = mc; c.indTexMtx[2 * m][2] = me; c.indTexMtx[2 * m][3] = -scale;
        c.indTexMtx[2 * m + 1][0] = mb; c.indTexMtx[2 * m + 1][1] = md; c.indTexMtx[2 * m + 1][2] = mf; c.indTexMtx[2 * m + 1][3] = -scale;
    }
    // z texture
    uint32_t ztype = BP(0xF5) & 3;
    static const int32_t zw[3][4] = { { 0, 0, 0, 1 }, { 1, 256, 0, 0 }, { 1, 256, 65536, 0 } };
    // component weights on (r,g,b,a) of the texture: u8 uses alpha? (hardware uses specific channels)
    if (ztype == 0) { c.zBias[0][0] = 0; c.zBias[0][1] = 0; c.zBias[0][2] = 0; c.zBias[0][3] = 1; }
    else if (ztype == 1) { c.zBias[0][0] = 256; c.zBias[0][1] = 0; c.zBias[0][2] = 0; c.zBias[0][3] = 1; }
    else { c.zBias[0][0] = 65536; c.zBias[0][1] = 256; c.zBias[0][2] = 1; c.zBias[0][3] = 0; }
    (void)zw;
    c.zBias[1][3] = BP(0xF4) & 0xFFFFFF;
    // fog
    uint32_t fa = BP(0xEE), fbm = BP(0xEF), fbs = BP(0xF0), fc = BP(0xF1);
    auto fogFloat = [](uint32_t v) {
        // 11-bit mantissa, 8-bit exponent, sign (s e8 m11) in 20 bits
        uint32_t m = v & 0x7FF, e = (v >> 11) & 0xFF, s = (v >> 19) & 1;
        uint32_t bits = (s << 31) | (e << 23) | (m << 12);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    };
    c.fogF[0] = fogFloat(fa);
    c.fogF[1] = fogFloat(fc);
    c.fogI[1] = fbm & 0xFFFFFF;
    c.fogI[3] = fbs & 31;
    uint32_t fcol = BP(0xF2);
    c.fogColor[0] = (fcol >> 16) & 0xFF;
    c.fogColor[1] = (fcol >> 8) & 0xFF;
    c.fogColor[2] = fcol & 0xFF;
    uint32_t rangeBase = BP(0xE8);
    float vpw = 2.0f * XFFloat(0x1000 + XF_VIEWPORT);
    if ((rangeBase >> 10) & 1)
    {
        int center = (int)(rangeBase & 0x3FF) - 342;
        float ssc = vpw != 0.0f ? center / vpw : 0.5f;
        c.fogF[2] = ssc * 2.0f - 1.0f;
        c.fogF[3] = vpw * (float)s_scale;
        for (int i = 0; i < 5; i++)
        {
            uint32_t r = BP(0xE9 + i);
            c.fogRange[(2 * i) / 4][(2 * i) % 4] = (float)((r >> 12) & 0xFFF) / 256.0f * 4.0f;           // LO
            c.fogRange[(2 * i + 1) / 4][(2 * i + 1) % 4] = (float)(r & 0xFFF) / 256.0f * 4.0f;           // HI
        }
    }
    else
    {
        c.fogF[2] = 0.0f;
        c.fogF[3] = 1.0f;
    }
    c.depthParams[0] = XFFloat(0x1000 + XF_VIEWPORT + 5);
    c.depthParams[1] = XFFloat(0x1000 + XF_VIEWPORT + 2);
}

int32_t g_tevKonst[4][4];
uint32_t g_framesPresented = 0;
uint32_t g_captureFrame = 0;     // frame number to log every draw of (0 = off)
static FILE* s_capture = nullptr;
static bool Capturing() { return g_captureFrame && g_framesPresented + 1 == g_captureFrame; }
static void CaptureLog(const std::string& line)
{
    if (!s_capture) s_capture = fopen("capture.txt", "w");
    if (s_capture) { fputs(line.c_str(), s_capture); fputc(10, s_capture); fflush(s_capture); }
}

template <typename T>
static void UpdateCB(ID3D11Buffer* b, const T& data)
{
    D3D11_MAPPED_SUBRESOURCE m;
    g_ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    memcpy(m.pData, &data, sizeof(T));
    g_ctx->Unmap(b, 0);
}

static UINT s_vbPos = 0, s_ibPos = 0;
static void EnsureBuffers(uint32_t vbytes, uint32_t ibytes)
{
    if (vbytes > s_vbCap)
    {
        if (s_vb) s_vb->Release();
        s_vbCap = std::max(vbytes, 32u << 20);
        s_vbPos = s_vbCap;   // force a discard on first use
        D3D11_BUFFER_DESC d{ s_vbCap, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
        g_device->CreateBuffer(&d, nullptr, &s_vb);
    }
    if (ibytes > s_ibCap)
    {
        if (s_ib) s_ib->Release();
        s_ibCap = std::max(ibytes, 8u << 20);
        s_ibPos = s_ibCap;
        D3D11_BUFFER_DESC d{ s_ibCap, D3D11_USAGE_DYNAMIC, D3D11_BIND_INDEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
        g_device->CreateBuffer(&d, nullptr, &s_ib);
    }
}

static void SetEFBTarget()
{
    g_ctx->OMSetRenderTargets(1, &s_efbRTV, s_efbDSV);
}

// ------------------------------------------------------------------ flush
static uint32_t s_drawCalls = 0;

void Render_Flush();
static void Flush()
{
    if (s_indices.empty() || !s_ready)
    {
        s_verts.clear();
        s_indices.clear();
        return;
    }
    uint32_t cull = (BP(0x00) >> 14) & 3;
    if (cull == 3 && s_topo == Topo::Tri)
    {
        s_verts.clear();
        s_indices.clear();
        return;
    }

    VSKey vk;
    BuildVSKey(vk);
    PSKey pk;
    BuildPSKey(pk);
    ID3D11VertexShader* vs = GetVS(vk);
    ID3D11PixelShader* ps = GetPS(pk);
    if (!vs || !ps)
    {
        s_verts.clear();
        s_indices.clear();
        return;
    }

    // XF memory
    if (s_xfDirty)
    {
        g_ctx->UpdateSubresource(s_xfBuf, 0, nullptr, g_gx.xfmem, 0, 0);
        s_xfDirty = false;
    }

    // viewport
    float wd = XFFloat(0x1000 + XF_VIEWPORT + 0);
    float ht = XFFloat(0x1000 + XF_VIEWPORT + 1);
    float xOrig = XFFloat(0x1000 + XF_VIEWPORT + 3);
    float yOrig = XFFloat(0x1000 + XF_VIEWPORT + 4);
    uint32_t so = BP(0x59);
    float xoff = (float)((so & 0x3FF) * 2);
    float yoff = (float)(((so >> 10) & 0x3FF) * 2);
    float vx = xOrig - wd - xoff;
    float vy = yOrig + ht - yoff;
    float vw = 2.0f * wd;
    float vh = -2.0f * ht;
    float flipX = 1.0f, flipY = 1.0f;
    if (vw < 0) { vx += vw; vw = -vw; flipX = -1.0f; }
    if (vh < 0) { vy += vh; vh = -vh; flipY = -1.0f; }
    D3D11_VIEWPORT vp{ vx * s_scale, vy * s_scale, vw * s_scale, vh * s_scale, 0.0f, 1.0f };
    {
        static int logged = 0;
        if (logged < 6)
        {
            logged++;
            const uint32_t* pp = &g_gx.xfregs[XF_PROJECTION];
            LOG_INFO("GX vp: wd={} ht={} xo={} yo={} so={:x} -> ({},{},{},{}) proj type={} p={} {} {} {} {} {}", wd, ht, xOrig, yOrig, so, vx, vy, vw, vh, pp[6],
                     AsFloat(pp[0]), AsFloat(pp[1]), AsFloat(pp[2]), AsFloat(pp[3]), AsFloat(pp[4]), AsFloat(pp[5]));
        }
    }
    g_ctx->RSSetViewports(1, &vp);

    // scissor
    uint32_t tl = BP(0x20), br = BP(0x21);
    int sx0 = (int)((tl >> 12) & 0x7FF) - (int)xoff, sy0 = (int)(tl & 0x7FF) - (int)yoff;
    int sx1 = (int)((br >> 12) & 0x7FF) - (int)xoff + 1, sy1 = (int)(br & 0x7FF) - (int)yoff + 1;
    D3D11_RECT sr{ std::max(0, sx0) * s_scale, std::max(0, sy0) * s_scale, std::min(EFB_W, sx1) * s_scale, std::min(EFB_H, sy1) * s_scale };
    if (sr.right < sr.left) sr.right = sr.left;
    if (sr.bottom < sr.top) sr.bottom = sr.top;
    g_ctx->RSSetScissorRects(1, &sr);

    VSConstants vc;
    FillVSConstants(vc, &flipX, &flipY);
    UpdateCB(s_vsCB, vc);

    // textures
    uint32_t texW[8] = { 1, 1, 1, 1, 1, 1, 1, 1 }, texH[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    ID3D11ShaderResourceView* srvs[8];
    ID3D11SamplerState* samps[8];
    for (int i = 0; i < 8; i++) { srvs[i] = s_dummySRV; samps[i] = s_pointClamp; }
    uint32_t usedMaps = 0;
    for (uint32_t st = 0; st < pk.numTevStages; st++)
    {
        uint32_t ord = (pk.tevOrder[st / 2] >> ((st & 1) * 12)) & 0xFFF;
        if ((ord >> 6) & 1) usedMaps |= 1 << (ord & 7);
    }
    for (uint32_t is = 0; is < pk.numIndStages; is++)
        usedMaps |= 1 << ((pk.indOrder >> (6 * is)) & 7);
    if (pk.numTexGens == 0) usedMaps = 0;
    for (int m = 0; m < 8; m++)
    {
        if (!(usedMaps & (1 << m))) continue;
        bool mips;
        srvs[m] = GetTexture(m, &texW[m], &texH[m], &mips);
        if (!srvs[m]) srvs[m] = s_dummySRV;
        samps[m] = GetSampler(TexReg(m, 0x80, 0xA0), TexReg(m, 0x84, 0xA4), mips);
    }

    PSConstants pc;
    FillPSConstants(pc, texW, texH);
    UpdateCB(s_psCB, pc);

    // geometry
    uint32_t vbytes = (uint32_t)(s_verts.size() * sizeof(HostVertex));
    uint32_t ibytes = (uint32_t)(s_indices.size() * 4);
    EnsureBuffers(vbytes * 4, ibytes * 4);
    // Ring buffers: append with NO_OVERWRITE, discard only when wrapping, so the
    // driver does not have to rename a multi-megabyte buffer on every draw.
    D3D11_MAPPED_SUBRESOURCE m;
    UINT vbOffset = (s_vbPos + sizeof(HostVertex) - 1) / sizeof(HostVertex) * sizeof(HostVertex);
    bool vbWrap = vbOffset + vbytes > s_vbCap;
    if (vbWrap) vbOffset = 0;
    g_ctx->Map(s_vb, 0, vbWrap ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m);
    memcpy((uint8_t*)m.pData + vbOffset, s_verts.data(), vbytes);
    g_ctx->Unmap(s_vb, 0);
    s_vbPos = vbOffset + vbytes;
    UINT ibOffset = s_ibPos;
    bool ibWrap = ibOffset + ibytes > s_ibCap;
    if (ibWrap) ibOffset = 0;
    g_ctx->Map(s_ib, 0, ibWrap ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m);
    memcpy((uint8_t*)m.pData + ibOffset, s_indices.data(), ibytes);
    g_ctx->Unmap(s_ib, 0);
    s_ibPos = ibOffset + ibytes;

    UINT stride = sizeof(HostVertex), offset = vbOffset;
    g_ctx->IASetInputLayout(s_layout);
    g_ctx->IASetVertexBuffers(0, 1, &s_vb, &stride, &offset);
    g_ctx->IASetIndexBuffer(s_ib, DXGI_FORMAT_R32_UINT, ibOffset);
    g_ctx->IASetPrimitiveTopology(s_topo == Topo::Tri ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
                                  : s_topo == Topo::Line ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST : D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    g_ctx->VSSetShader(vs, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &s_vsCB);
    g_ctx->VSSetShaderResources(0, 1, &s_xfSRV);
    g_ctx->PSSetShader(ps, nullptr, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &s_psCB);
    g_ctx->PSSetShaderResources(0, 8, srvs);
    g_ctx->PSSetSamplers(0, 8, samps);
    g_ctx->RSSetState(GetRasterState(s_topo == Topo::Tri && !g_dbgNoCull ? cull : 0));
    g_ctx->OMSetBlendState(GetBlendState(), nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(GetDepthState(), 0);
    SetEFBTarget();
    if (Capturing())
    {
        std::string t;
        for (int m = 0; m < 8; m++)
            if (usedMaps & (1 << m))
            {
                uint32_t img0 = TexReg(m, 0x88, 0xA8), img3 = TexReg(m, 0x94, 0xB4);
                t += std::format(" t{}={:08x}:{}x{}f{}", m, (img3 & 0xFFFFFF) << 5, (img0 & 0x3FF) + 1, ((img0 >> 10) & 0x3FF) + 1, (img0 >> 20) & 15);
            }
        CaptureLog(std::format("draw idx={} topo={} vc={:x} tev={} gens={} ind={} blend={:06x} z={:02x} cull={} fog={:x} af={:x} pf={} cc0={:06x} ac0={:06x} cc1={:06x} ac1={:06x} vp=({},{},{},{}) proj={} chan={:x}/{:x}{}",
            s_indices.size(), (int)s_topo, s_batchComponents, pk.numTevStages, pk.numTexGens, pk.numIndStages, BP(0x41) & 0xFFFF, BP(0x40) & 0x1F, cull,
            pk.fogType, pk.alphaFunc, pk.pixelFormat, pk.colorComb[0], pk.alphaComb[0], pk.colorComb[1], pk.alphaComb[1], vx, vy, vw, vh,
            g_gx.xfregs[XF_PROJECTION + 6], vk.chan[0], vk.chan[2], t));
    }
    g_ctx->DrawIndexed((UINT)s_indices.size(), 0, 0);
    s_drawCalls++;

    s_verts.clear();
    s_indices.clear();
}

// ------------------------------------------------------------------ GX callbacks
void Render_Draw(int prim, int vat, uint32_t count, const uint8_t* data, const VertexLayout& layout)
{
    if (!s_ready) return;
    uint32_t comps = VertexLoader_Components(layout);
    Topo topo = prim == 5 || prim == 6 ? Topo::Line : prim == 7 ? Topo::Point : Topo::Tri;
    if (!s_indices.empty() && (topo != s_topo || comps != s_batchComponents))
        Flush();
    s_topo = topo;
    s_batchComponents = comps;

    uint32_t base = (uint32_t)s_verts.size();
    s_verts.resize(base + count);
    VertexLoader_Decode(layout, data, count, s_verts.data() + base, s_skip);
    const bool checkSkip = !s_skip.empty();
    auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (checkSkip && (s_skip[a] || s_skip[b] || s_skip[c])) return;
        s_indices.push_back(base + a);
        s_indices.push_back(base + b);
        s_indices.push_back(base + c);
    };
    switch (prim)
    {
    case 0: case 1:   // quads
        for (uint32_t i = 0; i + 3 < count; i += 4)
        {
            tri(i, i + 1, i + 2);
            tri(i, i + 2, i + 3);
        }
        break;
    case 2:   // triangles
        for (uint32_t i = 0; i + 2 < count; i += 3) tri(i, i + 1, i + 2);
        break;
    case 3:   // strip
        for (uint32_t i = 0; i + 2 < count; i++)
        {
            if (i & 1) tri(i + 1, i, i + 2);
            else tri(i, i + 1, i + 2);
        }
        break;
    case 4:   // fan
        for (uint32_t i = 1; i + 1 < count; i++) tri(0, i, i + 1);
        break;
    case 5:   // lines
        for (uint32_t i = 0; i + 1 < count; i += 2) { s_indices.push_back(base + i); s_indices.push_back(base + i + 1); }
        break;
    case 6:   // line strip
        for (uint32_t i = 0; i + 1 < count; i++) { s_indices.push_back(base + i); s_indices.push_back(base + i + 1); }
        break;
    case 7:   // points
        for (uint32_t i = 0; i < count; i++) s_indices.push_back(base + i);
        break;
    }
    if (s_verts.size() > 60000)
        Flush();
}

void Render_Flush() { Flush(); }

void Render_OnXF(uint32_t addr, uint32_t count)
{
    if (addr < 0x1000) s_xfDirty = true;
}

void Render_OnCP(uint32_t reg)
{
    // vertex format changes only affect future draws (layout is decoded per draw)
    if (reg >= 0x30 && reg < 0x50) Flush();
}

static void DoEFBCopy();

void Render_OnBP(uint32_t reg, uint32_t oldValue, uint32_t newValue)
{
    if (reg == 0x52)
    {
        DoEFBCopy();
        return;
    }
    if (reg == 0x65)   // TLUT load
    {
        uint32_t ramAddr = (g_gx.bp[0x64] & 0xFFFFFF) << 5;
        uint32_t tmemAddr = (newValue & 0x3FF) << 9;
        uint32_t count = ((newValue >> 10) & 0x7FF) * 32;
        if (tmemAddr + count <= sizeof(s_tmem))
            memcpy(s_tmem + tmemAddr, PhysPtr(ramAddr), count);
        return;
    }
    if (reg >= 0xE0 && reg <= 0xE7)
    {
        // TEV color/konst registers: type bit 23 selects konst
        uint32_t idx = (reg - 0xE0) >> 1;
        bool isKonst = (newValue >> 23) & 1;
        if (isKonst)
        {
            if ((reg & 1) == 0)
            {
                g_tevKonst[idx][0] = newValue & 0xFF;
                g_tevKonst[idx][3] = (newValue >> 12) & 0xFF;
            }
            else
            {
                g_tevKonst[idx][2] = newValue & 0xFF;
                g_tevKonst[idx][1] = (newValue >> 12) & 0xFF;
            }
            // konst writes do not change the color register itself
            g_gx.bp[reg] = oldValue;
        }
        return;
    }
}

// ------------------------------------------------------------------ EFB copy / clear / present
struct UtilCB { float rect[4]; float uv[4]; float color[4]; float params[4]; int32_t iparams[4]; };

static void DrawQuad(ID3D11PixelShader* ps, const UtilCB& cb, ID3D11BlendState* bs, ID3D11DepthStencilState* ds)
{
    UpdateCB(s_utilCB, cb);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_ctx->VSSetShader(s_quadVS, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &s_utilCB);
    g_ctx->PSSetShader(ps, nullptr, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &s_utilCB);
    g_ctx->RSSetState(GetRasterState(0));
    g_ctx->OMSetBlendState(bs, nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(ds, 0);
    g_ctx->Draw(4, 0);
}

static ID3D11BlendState* s_clearBS[4];
static ID3D11DepthStencilState* s_clearDS[2];

static void ClearEFB(int x0, int y0, int x1, int y1)
{
    bool colorUpd = (BP(0x41) >> 3) & 1;
    bool alphaUpd = ((BP(0x41) >> 4) & 1) && PixelFormat() == 1;
    bool zUpd = (BP(0x40) >> 4) & 1;
    if (!s_clearBS[0])
    {
        for (int i = 0; i < 4; i++)
        {
            D3D11_BLEND_DESC d{};
            d.RenderTarget[0].RenderTargetWriteMask = ((i & 1) ? (D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE) : 0) |
                                                      ((i & 2) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0);
            g_device->CreateBlendState(&d, &s_clearBS[i]);
        }
        for (int i = 0; i < 2; i++)
        {
            D3D11_DEPTH_STENCIL_DESC d{};
            d.DepthEnable = i ? TRUE : FALSE;
            d.DepthWriteMask = i ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            d.DepthFunc = D3D11_COMPARISON_ALWAYS;
            g_device->CreateDepthStencilState(&d, &s_clearDS[i]);
        }
    }
    uint32_t ar = BP(0x4F), gb = BP(0x50);
    UtilCB cb{};
    cb.color[0] = (ar & 0xFF) / 255.0f;
    cb.color[3] = ((ar >> 8) & 0xFF) / 255.0f;
    cb.color[1] = ((gb >> 8) & 0xFF) / 255.0f;
    cb.color[2] = (gb & 0xFF) / 255.0f;
    if (PixelFormat() != 1) cb.color[3] = 1.0f;
    cb.params[0] = (float)(BP(0x51) & 0xFFFFFF) / 16777216.0f;
    cb.rect[0] = -1; cb.rect[1] = 1; cb.rect[2] = 1; cb.rect[3] = -1;
    D3D11_VIEWPORT vp{ 0, 0, (float)EFB_W * s_scale, (float)EFB_H * s_scale, 0, 1 };
    g_ctx->RSSetViewports(1, &vp);
    D3D11_RECT sr{ x0 * s_scale, y0 * s_scale, x1 * s_scale, y1 * s_scale };
    g_ctx->RSSetScissorRects(1, &sr);
    SetEFBTarget();
    int bsIdx = (colorUpd ? 1 : 0) | (alphaUpd ? 2 : 0);
    if (PixelFormat() != 1 && colorUpd) bsIdx |= 2;   // keep alpha at 1 in formats without alpha
    DrawQuad(s_clearPS, cb, s_clearBS[bsIdx], s_clearDS[zUpd ? 1 : 0]);
}

void Render_PresentXFB(ID3D11ShaderResourceView* srv, float u0, float v0, float u1, float v1);
static ID3D11Texture2D* s_xfbTex = nullptr;
static ID3D11RenderTargetView* s_xfbRTV = nullptr;
static ID3D11ShaderResourceView* s_xfbSRV = nullptr;
int g_screenshotEvery = 0;   // frames between automatic screenshots (0 = off)
uint32_t g_screenshotStart = 0;

static void SaveTextureBMP(ID3D11Texture2D* tex, const char* path)
{
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &st))) return;
    g_ctx->CopyResource(st, tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)))
    {
        // downscale 2x for smaller files
        uint32_t w = d.Width / 2, h = d.Height / 2;
        FILE* f = nullptr;
        if (!fopen_s(&f, path, "wb") && f)
        {
            uint32_t rowBytes = w * 3, pad = (4 - rowBytes % 4) % 4, img = (rowBytes + pad) * h;
            uint8_t hdr[54] = { 'B', 'M' };
            auto put32 = [&](int o, uint32_t v) { memcpy(hdr + o, &v, 4); };
            put32(2, 54 + img); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
            hdr[26] = 1; hdr[28] = 24; put32(34, img);
            fwrite(hdr, 1, 54, f);
            std::vector<uint8_t> row(rowBytes + pad);
            for (int y = (int)h - 1; y >= 0; y--)
            {
                const uint8_t* src = (const uint8_t*)m.pData + (size_t)(y * 2) * m.RowPitch;
                for (uint32_t x = 0; x < w; x++)
                {
                    row[x * 3 + 0] = src[x * 8 + 2];
                    row[x * 3 + 1] = src[x * 8 + 1];
                    row[x * 3 + 2] = src[x * 8 + 0];
                }
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
        }
        g_ctx->Unmap(st, 0);
    }
    st->Release();
}

static void DoEFBCopy()
{
    uint32_t ctl = BP(0x52);
    uint32_t src = BP(0x49), size = BP(0x4A);
    int sx = src & 0x3FF, sy = (src >> 10) & 0x3FF;
    int sw = (size & 0x3FF) + 1, sh = ((size >> 10) & 0x3FF) + 1;
    bool toXFB = (ctl >> 14) & 1;
    bool clear = (ctl >> 11) & 1;
    bool half = (ctl >> 9) & 1;
    uint32_t destAddr = (BP(0x4B) & 0xFFFFFF) << 5;

    // snapshot EFB for sampling
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g_ctx->CopyResource(s_efbColorCopy, s_efbColor);
    g_ctx->CopyResource(s_efbDepthCopy, s_efbDepth);

    float u0 = (float)sx / EFB_W, v0 = (float)sy / EFB_H, u1 = (float)(sx + sw) / EFB_W, v1 = (float)(sy + sh) / EFB_H;

    if (Capturing())
        CaptureLog(std::format("efbcopy ctl={:06x} src=({},{} {}x{}) dest={:08x} xfb={} clear={} pf={}", ctl, sx, sy, sw, sh, destAddr, toXFB, clear, PixelFormat()));
    if (toXFB)
    {
        uint32_t w = sw * s_scale, h = sh * s_scale;
        D3D11_TEXTURE2D_DESC td{};
        if (s_xfbTex) s_xfbTex->GetDesc(&td);
        if (!s_xfbTex || td.Width != w || td.Height != h)
        {
            if (s_xfbSRV) s_xfbSRV->Release();
            if (s_xfbRTV) s_xfbRTV->Release();
            if (s_xfbTex) s_xfbTex->Release();
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            g_device->CreateTexture2D(&d, nullptr, &s_xfbTex);
            g_device->CreateRenderTargetView(s_xfbTex, nullptr, &s_xfbRTV);
            g_device->CreateShaderResourceView(s_xfbTex, nullptr, &s_xfbSRV);
        }
        D3D11_BOX box{ (UINT)(sx * s_scale), (UINT)(sy * s_scale), 0, (UINT)((sx + sw) * s_scale), (UINT)((sy + sh) * s_scale), 1 };
        g_ctx->CopySubresourceRegion(s_xfbTex, 0, 0, 0, 0, s_efbColor, 0, &box);
        Video_Present(s_xfbSRV, 0, 0, 1, 1);
        g_framesPresented++;
        if (g_screenshotEvery && g_framesPresented >= g_screenshotStart && (g_framesPresented % g_screenshotEvery) == 0)
        {
            char path[64];
            snprintf(path, sizeof(path), "shot_%04u.bmp", g_framesPresented / g_screenshotEvery);
            SaveTextureBMP(s_xfbTex, path);
        }
        s_frame++;
        if ((g_framesPresented % 300) == 0)
            LOG_INFO("GX: {} frames, {} shaders (vs {} / ps {}), {} textures", g_framesPresented, s_vsCache.size() + s_psCache.size(),
                     s_vsCache.size(), s_psCache.size(), s_texCache.size());
    }
    else
    {
        // copy to texture
        uint32_t fmt = ((ctl >> 4) & 7) * 2 + ((ctl >> 3) & 1) * 0;   // placeholder, replaced below
        uint32_t tp = (ctl >> 3) & 0xF;
        fmt = (tp >> 1) | ((tp & 1) << 3);
        bool intensity = (ctl >> 15) & 1;
        bool depthSrc = PixelFormat() == 3;
        uint32_t tw = half ? std::max(1, sw / 2) : sw;
        uint32_t th = half ? std::max(1, sh / 2) : sh;
        uint32_t rw = tw * s_scale, rh = th * s_scale;

        TexEntry& e = s_efbCopies[destAddr];
        if (!e.tex || e.w != rw || e.h != rh)
        {
            ReleaseTex(e);
            CreateTexture(e, rw, rh, 1, nullptr, true);
        }
        ID3D11RenderTargetView* rtv = nullptr;
        g_device->CreateRenderTargetView(e.tex, nullptr, &rtv);
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{ 0, 0, (float)rw, (float)rh, 0, 1 };
        g_ctx->RSSetViewports(1, &vp);
        D3D11_RECT sr{ 0, 0, (LONG)rw, (LONG)rh };
        g_ctx->RSSetScissorRects(1, &sr);
        UtilCB cb{};
        cb.rect[0] = -1; cb.rect[1] = 1; cb.rect[2] = 1; cb.rect[3] = -1;
        cb.uv[0] = u0; cb.uv[1] = v0; cb.uv[2] = u1; cb.uv[3] = v1;
        cb.params[2] = 1.0f / EFB_W;
        cb.params[3] = 1.0f / EFB_H;
        cb.iparams[0] = fmt;
        cb.iparams[1] = intensity && fmt < 4 ? 1 : 0;
        cb.iparams[2] = depthSrc ? 1 : 0;
        cb.iparams[3] = half ? 1 : 0;
        ID3D11ShaderResourceView* srvs[2] = { s_efbColorCopySRV, s_efbDepthCopySRV };
        g_ctx->PSSetShaderResources(0, 2, srvs);
        g_ctx->PSSetSamplers(0, 1, half ? &s_linearClamp : &s_linearClamp);
        if (!s_clearBS[0]) ClearEFB(0, 0, 0, 0);   // creates helper states
        DrawQuad(s_copyPS, cb, s_clearBS[3], s_clearDS[0]);
        ID3D11ShaderResourceView* nulls[2] = { nullptr, nullptr };
        g_ctx->PSSetShaderResources(0, 2, nulls);
        rtv->Release();
        // remember the RAM content so later CPU-side uploads invalidate the copy
        static const uint32_t bppTable[16] = { 4, 8, 8, 16, 16, 16, 32, 8, 8, 8, 8, 16, 16, 16, 16, 16 };
        e.ramSize = std::max<uint32_t>(32, tw * th * bppTable[fmt & 15] / 8);
        e.ramHash = Hash64(PhysPtr(destAddr), e.ramSize);
        e.hashedFrame = s_frame;
        e.efbCopy = true;
        e.lastFrame = s_frame;
        if (Capturing())
        {
            char path[64];
            snprintf(path, sizeof(path), "efb_%08x_f%u.bmp", destAddr, fmt);
            SaveTextureBMP(e.tex, path);
        }
        // also report actual texel dimensions (unscaled) through w/h used for texDim
        e.w = rw;
        e.h = rh;
    }

    if (clear)
        ClearEFB(sx, sy, sx + sw, sy + sh);
    SetEFBTarget();
}

uint32_t EFB_Read32(uint32_t ea)
{
    // CPU EFB peeks (rare): report "far" depth / opaque black
    if (ea & 0x00400000) return 0x00FFFFFF;
    return 0xFF000000;
}

void EFB_Write32(uint32_t, uint32_t) {}
