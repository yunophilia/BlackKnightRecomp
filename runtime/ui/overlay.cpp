#include "overlay.h"
#include "settings_ui.h"
#include "ui_shot.h"
#include "../log.h"
#include "../tas/tas.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void App_Restart();
void Gamepad_StopRumble();

namespace
{
HWND s_hwnd = nullptr;
ID3D11Device* s_dev = nullptr;
ID3D11DeviceContext* s_ctx = nullptr;
bool s_ready = false;
std::atomic<bool> s_open{ false };
std::atomic<bool> s_homePress{ false };
std::mutex s_msgMutex;
std::vector<MSG> s_msgs;   // window messages, replayed on the render thread
// test aid: BKR_UI_SHOT_OVERLAY=<file.bmp> opens the overlay BKR_UI_SHOT_AFTER seconds
// (default 30) into the game, saves a screenshot a second later and closes it again
const char* s_shotPath = nullptr;
ULONGLONG s_shotOpenAt = 0;

bool IsInput(UINT msg)
{
    return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
           msg == WM_INPUTLANGCHANGE || msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_MOUSELEAVE;
}

std::string WindowsFont(const char* file)
{
    char dir[MAX_PATH];
    UINT n = GetWindowsDirectoryA(dir, MAX_PATH);
    return n ? std::string(dir, n) + "\\Fonts\\" + file : std::string();
}
}   // namespace

void Overlay_Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* ctx)
{
    s_shotPath = getenv("BKR_UI_SHOT_OVERLAY");
    if (Tas_Active() && !s_shotPath) return;   // scripted runs drive the game; no overlay
    if (s_shotPath)
    {
        const char* after = getenv("BKR_UI_SHOT_AFTER");
        s_shotOpenAt = GetTickCount64() + (after ? atoi(after) : 30) * 1000ull;
    }
    s_hwnd = hwnd;
    s_dev = device;
    s_ctx = ctx;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // nothing to remember between runs
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    std::string font = WindowsFont("segoeui.ttf");
    if (!font.empty() && GetFileAttributesA(font.c_str()) != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF(font.c_str(), 18.0f);
    SettingsUI_ApplyStyle();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device, ctx);
    s_ready = true;
}

bool Overlay_WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!s_ready || !IsInput(msg)) return false;
    if (msg == WM_KEYDOWN && (wp == VK_F1 || (wp == VK_ESCAPE && s_open)))
    {
        Overlay_Toggle();
        return true;
    }
    if (!s_open) return false;
    std::lock_guard lk(s_msgMutex);
    MSG m{};
    m.hwnd = hwnd;
    m.message = msg;
    m.wParam = wp;
    m.lParam = lp;
    s_msgs.push_back(m);
    return msg != WM_SETFOCUS && msg != WM_KILLFOCUS;   // the game sees no keys or clicks while open
}

void Overlay_Toggle()
{
    if (!s_ready) return;
    s_open = !s_open;
    LOG_INFO("settings overlay {}", s_open ? "opened" : "closed");
}

bool Overlay_IsOpen() { return s_open; }

bool Overlay_TakeHomePress() { return s_homePress.exchange(false); }

bool Overlay_ShotPending() { return s_shotPath && s_shotOpenAt && GetTickCount64() >= s_shotOpenAt; }

void Overlay_Render(float w, float h)
{
    if (!s_ready) return;
    {
        std::vector<MSG> msgs;
        {
            std::lock_guard lk(s_msgMutex);
            msgs.swap(s_msgs);
        }
        for (auto& m : msgs) ImGui_ImplWin32_WndProcHandler(m.hwnd, m.message, m.wParam, m.lParam);
    }
    if (s_shotPath && s_shotOpenAt && GetTickCount64() >= s_shotOpenAt && !s_open) s_open = true;
    if (!s_open) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::GetIO().DisplaySize = ImVec2(w, h);   // the bound target (an off-screen one for screenshots)
    // scale with the window: designed for 720p
    float scale = std::max(0.75f, ImGui::GetIO().DisplaySize.y / 720.0f);
    ImGui::GetStyle().FontScaleMain = scale;
    ImGui::NewFrame();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 size(std::min(vp->Size.x * 0.9f, 760.0f * scale), std::min(vp->Size.y * 0.85f, 560.0f * scale));
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    bool open = true;
    if (ImGui::Begin("Settings", &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                          ImGuiWindowFlags_NoSavedSettings))
    {
        float bottom = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        ImGui::BeginChild("panel", ImVec2(0, -bottom));
        SettingsUI_Draw(SettingsHost::InGame);
        ImGui::EndChild();
        if (ImGui::Button("Close")) open = false;
        ImGui::SameLine();
        if (ImGui::Button("Wii HOME Menu"))
        {
            s_homePress = true;   // the game opens it on the next controller read
            open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Restart game")) App_Restart();
        ImGui::SameLine();
        if (ImGui::Button("Quit game"))
        {
            LOG_INFO("quit from the settings overlay");
            Gamepad_StopRumble();
            TerminateProcess(GetCurrentProcess(), 0);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("View / F1 / Esc closes");
    }
    ImGui::End();
    if (!open) s_open = false;
    if (SettingsUI_TakeRestartRequest()) App_Restart();

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (s_shotPath && s_shotOpenAt && GetTickCount64() >= s_shotOpenAt + 1000)
    {
        UI_SaveBoundTarget(s_dev, s_ctx, s_shotPath);
        LOG_INFO("settings overlay screenshot saved to {}", s_shotPath);
        s_shotOpenAt = 0;
        s_open = false;
    }
}
