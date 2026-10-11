// Custom texture packs in Dolphin's format (see hires.h).
#include "hires.h"
#include "video.h"
#include "texture_decode.h"
#include "../log.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")

namespace fs = std::filesystem;

#define FOURCC(a, b, c, d) ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))

namespace
{
// ---------------------------------------------------------------- XXH64 (seed 0 use)
constexpr uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull,
                   P4 = 0x85EBCA77C2B2AE63ull, P5 = 0x27D4EB2F165667C5ull;
inline uint64_t Rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline uint64_t Rd64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }
inline uint32_t Rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline uint64_t Round(uint64_t acc, uint64_t in) { acc += in * P2; acc = Rotl(acc, 31); return acc * P1; }
inline uint64_t Merge(uint64_t acc, uint64_t v) { acc ^= Round(0, v); return acc * P1 + P4; }

uint64_t XXH64(const uint8_t* p, size_t len, uint64_t seed = 0)
{
    const uint8_t* end = p + len;
    uint64_t h;
    if (len >= 32)
    {
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const uint8_t* limit = end - 32;
        do
        {
            v1 = Round(v1, Rd64(p)); v2 = Round(v2, Rd64(p + 8)); v3 = Round(v3, Rd64(p + 16)); v4 = Round(v4, Rd64(p + 24));
            p += 32;
        } while (p <= limit);
        h = Rotl(v1, 1) + Rotl(v2, 7) + Rotl(v3, 12) + Rotl(v4, 18);
        h = Merge(h, v1); h = Merge(h, v2); h = Merge(h, v3); h = Merge(h, v4);
    }
    else
        h = seed + P5;
    h += len;
    while (p + 8 <= end) { h ^= Round(0, Rd64(p)); h = Rotl(h, 27) * P1 + P4; p += 8; }
    if (p + 4 <= end) { h ^= (uint64_t)Rd32(p) * P1; h = Rotl(h, 23) * P2 + P3; p += 4; }
    while (p < end) { h ^= (*p++) * P5; h = Rotl(h, 11) * P1; }
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

std::unordered_map<std::string, fs::path> s_files;                    // lowercase name -> file
std::unordered_map<std::string, ID3D11ShaderResourceView*> s_loaded;   // name -> texture (nullptr = failed)
IWICImagingFactory* s_wic = nullptr;

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

void Scan(const fs::path& dir)
{
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string ext = Lower(it->path().extension().string());
        if (ext != ".png" && ext != ".dds") continue;
        std::string stem = Lower(it->path().stem().string());
        if (stem.rfind("tex1_", 0) != 0 || stem.find("_mip") != std::string::npos) continue;
        // .dds preferred over .png when both exist
        auto [pos, inserted] = s_files.emplace(stem, it->path());
        if (!inserted && ext == ".dds") pos->second = it->path();
    }
}

ID3D11ShaderResourceView* MakeTexture(uint32_t w, uint32_t h, DXGI_FORMAT fmt, const std::vector<std::vector<uint8_t>>& levels,
                                      const std::vector<uint32_t>& pitches, bool genMips)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = genMips ? 0 : (UINT)levels.size();
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (genMips ? D3D11_BIND_RENDER_TARGET : 0);
    td.MiscFlags = genMips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    ID3D11Texture2D* tex = nullptr;
    if (genMips)
    {
        if (FAILED(g_device->CreateTexture2D(&td, nullptr, &tex))) return nullptr;
        g_ctx->UpdateSubresource(tex, 0, nullptr, levels[0].data(), pitches[0], 0);
    }
    else
    {
        std::vector<D3D11_SUBRESOURCE_DATA> init(levels.size());
        for (size_t i = 0; i < levels.size(); i++) init[i] = { levels[i].data(), pitches[i], 0 };
        if (FAILED(g_device->CreateTexture2D(&td, init.data(), &tex))) return nullptr;
    }
    ID3D11ShaderResourceView* srv = nullptr;
    g_device->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    if (genMips && srv) g_ctx->GenerateMips(srv);
    return srv;
}

ID3D11ShaderResourceView* LoadPNG(const fs::path& path, bool mipmapped)
{
    if (!s_wic) return nullptr;
    IWICBitmapDecoder* dec = nullptr;
    if (FAILED(s_wic->CreateDecoderFromFilename(path.wstring().c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)))
        return nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT w = 0, h = 0;
    if (SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(s_wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(conv->GetSize(&w, &h)) && w && h)
    {
        std::vector<std::vector<uint8_t>> lv(1, std::vector<uint8_t>((size_t)w * h * 4));
        if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)lv[0].size(), lv[0].data())))
            srv = MakeTexture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, lv, { w * 4 }, mipmapped);
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    dec->Release();
    return srv;
}

// Minimal DDS reader: BC1/BC2/BC3/BC7 (FourCC or DX10 header) and RGBA8, with mips.
ID3D11ShaderResourceView* LoadDDS(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0) return nullptr;
    auto u32 = [&](size_t o) { return Rd32(d.data() + o); };
    uint32_t h = u32(12), w = u32(16), mips = std::max(1u, u32(28));
    uint32_t pfFlags = u32(80), fourcc = u32(84), bpp = u32(88);
    size_t off = 128;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    if (pfFlags & 4)
    {
        if (fourcc == FOURCC('D', 'X', 'T', '1')) fmt = DXGI_FORMAT_BC1_UNORM;
        else if (fourcc == FOURCC('D', 'X', 'T', '3')) fmt = DXGI_FORMAT_BC2_UNORM;
        else if (fourcc == FOURCC('D', 'X', 'T', '5')) fmt = DXGI_FORMAT_BC3_UNORM;
        else if (fourcc == FOURCC('D', 'X', '1', '0') && d.size() >= 148)
        {
            fmt = (DXGI_FORMAT)u32(128);
            off = 148;
        }
    }
    else if ((pfFlags & 0x40) && bpp == 32)
        fmt = u32(92) == 0xFF ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    bool bc = false;
    uint32_t blockBytes = 0;
    switch (fmt)
    {
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: bc = true; blockBytes = 8; break;
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
    case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC3_UNORM_SRGB: bc = true; blockBytes = 16; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: break;
    default: return nullptr;
    }
    std::vector<std::vector<uint8_t>> lv;
    std::vector<uint32_t> pitches;
    uint32_t lw = w, lh = h;
    for (uint32_t i = 0; i < mips; i++)
    {
        uint32_t pitch = bc ? std::max(1u, (lw + 3) / 4) * blockBytes : lw * 4;
        uint32_t rows = bc ? std::max(1u, (lh + 3) / 4) : lh;
        size_t size = (size_t)pitch * rows;
        if (off + size > d.size()) break;
        lv.emplace_back(d.begin() + off, d.begin() + off + size);
        pitches.push_back(pitch);
        off += size;
        lw = std::max(1u, lw / 2);
        lh = std::max(1u, lh / 2);
    }
    if (lv.empty()) return nullptr;
    return MakeTexture(w, h, fmt, lv, pitches, false);
}
}   // namespace

void HiRes_Init(const std::string& configuredDir)
{
    std::vector<fs::path> roots;
    if (!configuredDir.empty())
        roots.push_back(configuredDir);
    else
    {
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        fs::path exe = fs::path(buf).parent_path();
        roots.push_back(exe / "textures");
        roots.push_back(exe / "game" / "textures");
        roots.push_back(exe / ".." / "game" / "textures");
        if (const char* up = getenv("USERPROFILE"))
            roots.push_back(fs::path(up) / "Documents" / "Dolphin Emulator" / "Load" / "Textures");
    }
    std::error_code ec;
    for (auto& r : roots)
        for (const char* id : { "RENE8P", "RENE", "REN" })
            if (fs::is_directory(r / id, ec)) Scan(r / id);
    if (s_files.empty()) return;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s_wic));
    LOG_INFO("texture pack: {} replacement textures", s_files.size());
}

bool HiRes_Enabled() { return !s_files.empty(); }

// Dolphin's texture names for this texture: [0] exact, [1] palette wildcard (CI only).
static void MakeNames(const uint8_t* data, uint32_t dataSize, const uint8_t* tlut, uint32_t tlutSize, uint32_t fmt, uint32_t w, uint32_t h,
                      bool mipmapped, std::string names[2])
{
    // Palette range actually referenced, exactly as Dolphin computes it (including its
    // byte-swap quirk for C14X2).
    uint32_t palSize = fmt == TF_C4 ? 32 : fmt == TF_C8 ? 512 : fmt == TF_C14X2 ? 32768 : 0;
    uint64_t tlutHash = 0;
    if (palSize)
    {
        uint32_t mn = 0xFFFF, mx = 0;
        if (fmt == TF_C4)
            for (uint32_t i = 0; i < dataSize; i++) { mn = std::min({ mn, data[i] & 15u, (uint32_t)data[i] >> 4 }); mx = std::max({ mx, data[i] & 15u, (uint32_t)data[i] >> 4 }); }
        else if (fmt == TF_C8)
            for (uint32_t i = 0; i < dataSize; i++) { mn = std::min(mn, (uint32_t)data[i]); mx = std::max(mx, (uint32_t)data[i]); }
        else
            for (uint32_t i = 0; i < dataSize; i += 2) { uint32_t v = _byteswap_ushort(data[i]) & 0x3FFF; mn = std::min(mn, v); mx = std::max(mx, v); }
        uint32_t used = 2 * (mx + 1 - mn);
        if (2 * mn + used > tlutSize) return;
        tlutHash = XXH64(tlut + 2 * mn, used);
    }
    std::string base = std::format("tex1_{}x{}{}_{:016x}", w, h, mipmapped ? "_m" : "", XXH64(data, dataSize));
    if (palSize)
    {
        names[0] = std::format("{}_{:016x}_{}", base, tlutHash, fmt);
        names[1] = std::format("{}_$_{}", base, fmt);
    }
    else
        names[0] = std::format("{}_{}", base, fmt);
}

ID3D11ShaderResourceView* HiRes_Find(const uint8_t* data, uint32_t dataSize, const uint8_t* tlut, uint32_t tlutSize,
                                     uint32_t fmt, uint32_t w, uint32_t h, bool mipmapped)
{
    std::string names[2];
    MakeNames(data, dataSize, tlut, tlutSize, fmt, w, h, mipmapped, names);
    for (auto& n : names)
    {
        if (n.empty()) continue;
        auto f = s_files.find(n);
        if (f == s_files.end()) continue;
        auto l = s_loaded.find(n);
        ID3D11ShaderResourceView* srv;
        if (l != s_loaded.end())
            srv = l->second;
        else
        {
            bool dds = Lower(f->second.extension().string()) == ".dds";
            srv = dds ? LoadDDS(f->second) : LoadPNG(f->second, mipmapped);
            if (!srv) LOG_WARN("texture pack: failed to load {}", f->second.string());
            s_loaded[n] = srv;
        }
        if (srv) srv->AddRef();
        return srv;
    }
    return nullptr;
}

bool g_dumpTextures = false;

void HiRes_Dump(const uint8_t* data, uint32_t dataSize, const uint8_t* tlut, uint32_t tlutSize, uint32_t fmt, uint32_t w, uint32_t h,
                bool mipmapped, const uint32_t* rgba)
{
    static std::unordered_map<std::string, bool> done;
    static fs::path dir;
    if (dir.empty())
    {
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        dir = fs::path(buf).parent_path() / "dump" / "textures" / "RENE8P";
        std::error_code ec;
        fs::create_directories(dir, ec);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (!s_wic) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s_wic));
    }
    std::string names[2];
    MakeNames(data, dataSize, tlut, tlutSize, fmt, w, h, mipmapped, names);
    if (names[0].empty() || done[names[0]] || !s_wic) return;
    done[names[0]] = true;
    fs::path path = dir / (names[0] + ".png");
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    if (SUCCEEDED(s_wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(s_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h)))
    {
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppRGBA;
        frame->SetPixelFormat(&pf);
        frame->WritePixels(h, w * 4, w * h * 4, (BYTE*)rgba);
        frame->Commit();
        enc->Commit();
    }
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
}
