// Window, D3D11 device and presentation.
#include "video.h"
#include "../config.h"
#include "../log.h"
#include "../ui/overlay.h"

#include <atomic>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <thread>
#include <windows.h>

VideoConfig g_videoConfig;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;

static HWND s_hwnd = nullptr;
static IDXGISwapChain1* s_swapchain = nullptr;
static ID3D11RenderTargetView* s_backbufferRTV = nullptr;
static std::atomic<bool> s_closed{ false };
static std::atomic<bool> s_resized{ false };
static ID3D11VertexShader* s_blitVS = nullptr;
static ID3D11PixelShader* s_blitPS = nullptr;
static ID3D11SamplerState* s_linearSampler = nullptr;
static ID3D11Buffer* s_blitCB = nullptr;
static ID3D11RasterizerState* s_blitRS = nullptr;
static ID3D11BlendState* s_blitBS = nullptr;
static ID3D11DepthStencilState* s_blitDS = nullptr;
static bool s_fullscreen = false;   // window thread only
// UI screenshots of a minimized test window (BKR_UI_SHOT_OVERLAY): drawn off screen
static ID3D11Texture2D* s_shotTex = nullptr;
static ID3D11RenderTargetView* s_shotRTV = nullptr;
constexpr UINT WM_APP_FULLSCREEN = WM_APP + 1;   // wParam: 1 fullscreen, 0 windowed
constexpr UINT WM_APP_WINDOWSIZE = WM_APP + 2;   // wParam: width, lParam: height

// borderless fullscreen on the window's monitor, or back to the saved window rect
static void SetFullscreen(HWND hwnd, bool on)
{
    static RECT saved;
    if (on == s_fullscreen) return;
    s_fullscreen = on;
    if (on)
    {
        GetWindowRect(hwnd, &saved);
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    }
    else
    {
        SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(hwnd, nullptr, saved.left, saved.top, saved.right - saved.left, saved.bottom - saved.top, SWP_FRAMECHANGED);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (Overlay_WndProc(hwnd, msg, wp, lp)) return 0;   // the settings overlay has the input
    switch (msg)
    {
    case WM_CLOSE:
        // Closing the window quits. Saves are written to the host NAND as the game
        // issues them, so there is nothing to flush; terminate directly rather than
        // running DLL teardown while guest/GX/audio threads are mid-operation.
        s_closed = true;
        LOG_INFO("window closed, exiting");
        { void Gamepad_StopRumble(); Gamepad_StopRumble(); }
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    case WM_SIZE:
        s_resized = true;
        return 0;
    case WM_ACTIVATEAPP:
        if (g_config.pauseOnFocusLoss)
        {
            extern std::atomic<bool> g_pauseRequested;
            g_pauseRequested = (wp == FALSE);
        }
        break;
    case WM_SETCURSOR:
        // the game draws its own Wii Remote pointer; the overlay needs the mouse cursor
        if (LOWORD(lp) == HTCLIENT)
        {
            SetCursor(Overlay_IsOpen() ? LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)) : nullptr);
            return TRUE;
        }
        break;
    case WM_APP_FULLSCREEN:
        SetFullscreen(hwnd, wp != 0);
        return 0;
    case WM_APP_WINDOWSIZE:
        if (!s_fullscreen)
        {
            RECT r{ 0, 0, (LONG)wp, (LONG)lp };
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_F11 || (wp == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000)))
        {
            SetFullscreen(hwnd, !s_fullscreen);
            g_config.fullscreen = s_fullscreen;
        }
        else if (wp == VK_F1)
            Overlay_Toggle();
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) { SendMessageW(hwnd, WM_KEYDOWN, VK_RETURN, lp); return 0; }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void WindowThread(std::atomic<int>* ready)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"BlackKnightRecomp";
    RegisterClassW(&wc);
    RECT r{ 0, 0, g_videoConfig.windowWidth, g_videoConfig.windowHeight };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    s_hwnd = CreateWindowW(L"BlackKnightRecomp", L"Sonic and the Black Knight - Recompiled", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (s_hwnd && g_videoConfig.fullscreen) SetFullscreen(s_hwnd, true);
    *ready = s_hwnd ? 1 : -1;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static ID3DBlob* Compile(const char* src, const char* entry, const char* target)
{
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (FAILED(hr))
    {
        LOG_ERROR("shader compile failed: {}", err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return nullptr;
    }
    if (err) err->Release();
    return blob;
}

static const char* kBlitHLSL = R"(
cbuffer C : register(b0) { float4 uvRect; float4 dstRect; };
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o;
    float2 t = float2((id << 1) & 2, id & 2) * 0.5;   // 0..1 quad corners (triangle strip of 4)
    t = float2(id & 1, id >> 1);
    o.pos = float4(lerp(dstRect.x, dstRect.z, t.x), lerp(dstRect.y, dstRect.w, t.y), 0, 1);
    o.uv = lerp(uvRect.xy, uvRect.zw, t);
    return o;
}
float4 ps(V i) : SV_Target { return float4(tex.Sample(samp, i.uv).rgb, 1.0); }
)";

static void CreateBackbufferRTV()
{
    ID3D11Texture2D* bb = nullptr;
    s_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    g_device->CreateRenderTargetView(bb, nullptr, &s_backbufferRTV);
    bb->Release();
}

bool Video_Init()
{
    std::atomic<int> ready{ 0 };
    std::thread(WindowThread, &ready).detach();
    while (ready == 0) Sleep(1);
    if (ready < 0) return false;

    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &fl, 1, D3D11_SDK_VERSION, &g_device, nullptr, &g_ctx);
    if (FAILED(hr))
    {
        LOG_ERROR("D3D11CreateDevice failed {:08x}", (uint32_t)hr);
        return false;
    }
    IDXGIDevice* dxgiDev = nullptr;
    g_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
    IDXGIAdapter* adapter = nullptr;
    dxgiDev->GetAdapter(&adapter);
    IDXGIFactory2* factory = nullptr;
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    DXGI_ADAPTER_DESC ad;
    adapter->GetDesc(&ad);
    char name[128];
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name), nullptr, nullptr);
    LOG_INFO("D3D11 adapter: {}", name);

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 0;
    sd.Height = 0;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = factory->CreateSwapChainForHwnd(g_device, s_hwnd, &sd, nullptr, nullptr, &s_swapchain);
    if (FAILED(hr))
    {
        LOG_ERROR("CreateSwapChainForHwnd failed {:08x}", (uint32_t)hr);
        return false;
    }
    factory->MakeWindowAssociation(s_hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->Release();
    adapter->Release();
    dxgiDev->Release();
    CreateBackbufferRTV();

    ID3DBlob* vs = Compile(kBlitHLSL, "vs", "vs_5_0");
    ID3DBlob* ps = Compile(kBlitHLSL, "ps", "ps_5_0");
    if (!vs || !ps) return false;
    g_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &s_blitVS);
    g_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &s_blitPS);
    vs->Release();
    ps->Release();
    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    g_device->CreateSamplerState(&smp, &s_linearSampler);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 32;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_device->CreateBuffer(&bd, nullptr, &s_blitCB);
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    g_device->CreateRasterizerState(&rs, &s_blitRS);
    D3D11_BLEND_DESC bs{};
    bs.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_device->CreateBlendState(&bs, &s_blitBS);
    D3D11_DEPTH_STENCIL_DESC ds{};
    g_device->CreateDepthStencilState(&ds, &s_blitDS);
    Overlay_Init(s_hwnd, g_device, g_ctx);
    return true;
}

void Video_Present(ID3D11ShaderResourceView* srv, float u0, float v0, float u1, float v1)
{
    if (s_resized.exchange(false))
    {
        g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        if (s_backbufferRTV) s_backbufferRTV->Release();
        s_swapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
        CreateBackbufferRTV();
    }
    RECT rc;
    GetClientRect(s_hwnd, &rc);
    float w = (float)std::max<LONG>(1, rc.right - rc.left);
    float h = (float)std::max<LONG>(1, rc.bottom - rc.top);
    ID3D11RenderTargetView* target = s_backbufferRTV;
    if (Overlay_ShotPending() && w < 64)
    {
        if (!s_shotTex)
        {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = 1280; td.Height = 720; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            g_device->CreateTexture2D(&td, nullptr, &s_shotTex);
            g_device->CreateRenderTargetView(s_shotTex, nullptr, &s_shotRTV);
        }
        target = s_shotRTV;
        w = 1280;
        h = 720;
    }
    D3D11_VIEWPORT vp{ 0, 0, w, h, 0, 1 };
    g_ctx->RSSetViewports(1, &vp);
    float black[4] = { 0, 0, 0, 1 };
    g_ctx->OMSetRenderTargets(1, &target, nullptr);
    g_ctx->ClearRenderTargetView(target, black);
    if (srv)
    {
        // letterbox to target aspect
        float aspect = g_videoConfig.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
        float dw = w, dh = h;
        if (w / h > aspect) dw = h * aspect;
        else dh = w / aspect;
        float x0 = (w - dw) / 2, y0 = (h - dh) / 2;
        float cb[8] = { u0, v0, u1, v1,
                        x0 / w * 2 - 1, 1 - y0 / h * 2, (x0 + dw) / w * 2 - 1, 1 - (y0 + dh) / h * 2 };
        D3D11_MAPPED_SUBRESOURCE m;
        g_ctx->Map(s_blitCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        memcpy(m.pData, cb, sizeof(cb));
        g_ctx->Unmap(s_blitCB, 0);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        g_ctx->VSSetShader(s_blitVS, nullptr, 0);
        g_ctx->VSSetConstantBuffers(0, 1, &s_blitCB);
        g_ctx->PSSetShader(s_blitPS, nullptr, 0);
        g_ctx->PSSetShaderResources(0, 1, &srv);
        g_ctx->PSSetSamplers(0, 1, &s_linearSampler);
        g_ctx->RSSetState(s_blitRS);
        g_ctx->OMSetBlendState(s_blitBS, nullptr, 0xFFFFFFFF);
        g_ctx->OMSetDepthStencilState(s_blitDS, 0);
        g_ctx->GSSetShader(nullptr, nullptr, 0);
        g_ctx->Draw(4, 0);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        g_ctx->PSSetShaderResources(0, 1, &nullSrv);
    }
    Overlay_Render(w, h);   // in-game settings, drawn over the game while it keeps running
    s_swapchain->Present(g_videoConfig.vsync ? 1 : 0, 0);
}

bool Video_WindowClosed() { return s_closed; }
void Video_SetFullscreen(bool on) { if (s_hwnd) PostMessageW(s_hwnd, WM_APP_FULLSCREEN, on ? 1 : 0, 0); }
void Video_SetWindowSize(int w, int h) { if (s_hwnd) PostMessageW(s_hwnd, WM_APP_WINDOWSIZE, (WPARAM)w, (LPARAM)h); }
void* Video_HWND() { return s_hwnd; }
