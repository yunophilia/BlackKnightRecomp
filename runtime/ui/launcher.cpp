// Launcher shown on every start: game data status (install / repair from the disc image,
// full file check), settings, and Play. Its own small window and D3D11 device; both are
// gone before the game creates its window.
#include "settings_ui.h"
#include "ui_shot.h"
#include "../config.h"
#include "../install/verify.h"
#include "../log.h"
#include "../platform/platform.h"
#include "../platform/ui.h"
#include "../region.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <atomic>
#include <d3d11.h>
#include <dxgi.h>
#include <cstring>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

bool InstallUI_Run(const fs::path& dest, const std::string& foundImage, const std::string& damage);

namespace
{
ID3D11Device* s_dev = nullptr;
ID3D11DeviceContext* s_ctx = nullptr;
IDXGISwapChain* s_swap = nullptr;
ID3D11RenderTargetView* s_rtv = nullptr;
bool s_quit = false;

void CreateRTV()
{
    ID3D11Texture2D* bb = nullptr;
    s_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    s_dev->CreateRenderTargetView(bb, nullptr, &s_rtv);
    bb->Release();
}

struct State
{
    fs::path data;
    DataStatus status = DataStatus::Missing;
    std::string problem;
    std::string release;            // "USA (RENE8P)" once installed
    std::string releaseError;       // this build cannot play the installed release
    bool promptInstall = false;     // ask for the disc image after the window is shown
    std::thread verifyThread;
    VerifyProgress verify;
    std::atomic<bool> verifying{ false };
    std::string verifyResult;
};

void Check(State& st)
{
    st.status = Install_QuickCheck(st.data, st.problem);
    st.release.clear();
    st.releaseError.clear();
    if (st.status == DataStatus::Ok)
    {
        std::string id = Release_InstalledId(st.data);
        if (const GameRelease* r = Release_Find(id)) st.release = std::string(r->name) + " (" + id + ")";
        Region_Select(id, st.releaseError);
    }
    LOG_INFO("launcher: game data {} {}", st.data.string(),
             st.status == DataStatus::Ok ? "ok" : st.status == DataStatus::Missing ? "not installed" : "damaged: " + st.problem);
}

void StartVerify(State& st)
{
    if (st.verifying) return;
    if (st.verifyThread.joinable()) st.verifyThread.join();
    st.verify.cancel = false;
    st.verifying = true;
    st.verifyResult.clear();
    st.verifyThread = std::thread([&st] {
        std::string problem;
        DataStatus r = Install_FullCheck(st.data, st.verify, problem);
        if (st.verify.cancel) st.verifyResult = "Check canceled.";
        else if (r == DataStatus::Ok) st.verifyResult = "All game files are intact.";
        else
        {
            st.verifyResult = "Damaged: " + problem;
            st.status = r;
            st.problem = problem;
        }
        st.verifying = false;
    });
}

void DrawHome(State& st, bool& play, const std::string& isoPath, const fs::path& installDest)
{
    ImGui::Spacing();
    const ImVec4 green(0.45f, 0.80f, 0.40f, 1), red(0.90f, 0.40f, 0.30f, 1);
    switch (st.status)
    {
    case DataStatus::Ok: ImGui::TextColored(green, "Game data installed: %s", st.release.c_str()); break;
    case DataStatus::Missing: ImGui::TextColored(red, "Game data not installed"); break;
    case DataStatus::Corrupt: ImGui::TextColored(red, "Game data damaged: %s", st.problem.c_str()); break;
    }
    if (!st.releaseError.empty()) ImGui::TextColored(red, "%s", st.releaseError.c_str());
    ImGui::TextDisabled("%s", st.data.string().c_str());
    ImGui::Spacing();

    bool ready = st.status == DataStatus::Ok && st.releaseError.empty() && !st.verifying;
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("  Play  ", ImVec2(ImGui::GetFontSize() * 10, ImGui::GetFontSize() * 2.4f))) play = true;
    ImGui::EndDisabled();
    if (ready && !ImGui::IsAnyItemActive() && ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);

    ImGui::Spacing();
    ImGui::SeparatorText("Game data");
    ImGui::BeginDisabled(st.verifying);
    if (ImGui::Button(st.status == DataStatus::Ok ? "Reinstall from disc image..." : "Install from disc image..."))
    {
        fs::path dest = st.status == DataStatus::Missing ? installDest : st.data;
        if (InstallUI_Run(dest, isoPath, st.status == DataStatus::Corrupt ? st.problem : ""))
            st.data = dest;
        Check(st);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(st.status == DataStatus::Missing);
    if (ImGui::Button("Check all game files")) StartVerify(st);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (st.verifying)
    {
        uint64_t total = st.verify.total ? st.verify.total.load() : 1;
        ImGui::ProgressBar((float)((double)st.verify.done / total), ImVec2(ImGui::GetFontSize() * 20, 0), "Checking...");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) st.verify.cancel = true;
    }
    else if (!st.verifyResult.empty())
        ImGui::TextWrapped("%s", st.verifyResult.c_str());
    ImGui::TextDisabled("Install reads your own disc image (ISO, RVZ, WIA, WBFS); it is not needed afterwards.");

    ImGui::Spacing();
    ImGui::SeparatorText("Controls");
    ImGui::TextDisabled("In game: View (Back) or F1 opens the settings. View + Start is the Wii HOME button.");
}
}   // namespace

// Returns true to start the game, with dataDir set to the installed data.
bool Launcher_Run(std::string& dataDir, const std::string& isoPath, const fs::path& installDest)
{
    HWND hwnd = (HWND)UiWindow_Create("Sonic and the Black Knight - Recompiled", 900, 640);
    if (!hwnd)
    {
        LOG_WARN("launcher: no window, starting the game directly");
        return true;
    }
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd,
                                             &s_swap, &s_dev, nullptr, &s_ctx)))
    {
        LOG_WARN("launcher: no D3D11 device, starting the game directly");
        UiWindow_Destroy();
        return true;
    }
    CreateRTV();
    UiWindow_Show();

    IMGUI_CHECKVERSION();
    ImGuiContext* imgui = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    std::string font = Platform_UiFont();
    if (!font.empty()) io.Fonts->AddFontFromFileTTF(font.c_str(), 18.0f);
    SettingsUI_ApplyStyle();
    UiBackend_Init(hwnd);
    ImGui_ImplDX11_Init(s_dev, s_ctx);

    State st;
    st.data = dataDir.empty() ? installDest : fs::path(dataDir);
    Check(st);
    // test aid: BKR_UI_SHOT=<file.bmp> saves the launcher (BKR_UI_SHOT_TAB=settings: the
    // settings tab) and quits, without asking for a disc image
    const char* shot = getenv("BKR_UI_SHOT");
    const char* shotTab = getenv("BKR_UI_SHOT_TAB");
    st.promptInstall = st.status != DataStatus::Ok && !shot;

    bool play = false;
    s_quit = false;
    int frames = 0;
    while (!s_quit && !play)
    {
        uint32_t resizeW, resizeH;
        if (!UiWindow_Pump(resizeW, resizeH)) s_quit = true;
        if (s_quit) break;
        if (resizeW)
        {
            if (s_rtv) { s_rtv->Release(); s_rtv = nullptr; }
            s_swap->ResizeBuffers(0, resizeW, resizeH, DXGI_FORMAT_UNKNOWN, 0);
            CreateRTV();
        }
        // first start, deleted or damaged data: ask for the disc image once the window is up
        if (st.promptInstall && frames++ > 2)
        {
            st.promptInstall = false;
            fs::path dest = st.status == DataStatus::Missing ? installDest : st.data;
            if (InstallUI_Run(dest, isoPath, st.status == DataStatus::Corrupt ? st.problem : "")) st.data = dest;
            Check(st);
        }

        ImGui_ImplDX11_NewFrame();
        UiBackend_NewFrame();
        if (shot)
        {
            // screenshots run minimized (no client area): lay out for the back buffer
            DXGI_SWAP_CHAIN_DESC d;
            s_swap->GetDesc(&d);
            io.DisplaySize = ImVec2((float)d.BufferDesc.Width, (float)d.BufferDesc.Height);
        }
        ImGui::NewFrame();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.6f);
        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "Sonic and the Black Knight");
        ImGui::PopFont();
        ImGui::TextDisabled("Recompiled for PC");
        float bottom = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        ImGui::BeginChild("tabs", ImVec2(0, -bottom));
        if (ImGui::BeginTabBar("launcherTabs"))
        {
            if (ImGui::BeginTabItem("Play"))
            {
                DrawHome(st, play, isoPath, installDest);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings", nullptr,
                                    frames < 2 && shotTab && !strcmp(shotTab, "settings") ? ImGuiTabItemFlags_SetSelected : 0))
            {
                SettingsUI_Draw(SettingsHost::Launcher);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        if (ImGui::Button("Quit")) s_quit = true;
        SettingsUI_DrawVersion();
        ImGui::End();

        ImGui::Render();
        const float clear[4] = { 0.06f, 0.06f, 0.05f, 1 };
        s_ctx->OMSetRenderTargets(1, &s_rtv, nullptr);
        s_ctx->ClearRenderTargetView(s_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (shot && ++frames > 5)
        {
            UI_SaveBoundTarget(s_dev, s_ctx, shot);
            s_quit = true;
        }
        s_swap->Present(1, 0);
    }

    st.verify.cancel = true;
    if (st.verifyThread.joinable()) st.verifyThread.join();
    ImGui_ImplDX11_Shutdown();
    UiBackend_Shutdown();
    ImGui::DestroyContext(imgui);
    if (s_rtv) s_rtv->Release();
    s_swap->Release();
    s_ctx->Release();
    s_dev->Release();
    s_rtv = nullptr;
    UiWindow_Destroy();
    if (play) dataDir = st.data.string();
    LOG_INFO("launcher: {}", play ? "play" : "quit");
    return play;
}
