#include "overlay.h"
#include "settings_ui.h"
#include "ui_image.h"
#include "ui_shot.h"
#include "../log.h"
#include "../platform/platform.h"
#include "../platform/ui.h"
#include "../tas/tas.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <algorithm>
#include <atomic>
#include <string>

void App_Restart();
void Gamepad_StopRumble();

namespace
{
ID3D11Device* s_dev = nullptr;
ID3D11DeviceContext* s_ctx = nullptr;
std::atomic<bool> s_ready{ false };
std::atomic<bool> s_open{ false };
std::atomic<bool> s_homePress{ false };
// test aid: BKR_UI_SHOT_OVERLAY=<file.bmp> opens the overlay BKR_UI_SHOT_AFTER seconds
// (default 30) into the game, saves a screenshot BKR_UI_SHOT_HOLD seconds later (default 1)
// and closes it again
const char* s_shotPath = nullptr;
uint64_t s_shotOpenAt = 0;
uint64_t s_shotHold = 1000;
}   // namespace

void Overlay_Init(void* window, ID3D11Device* device, ID3D11DeviceContext* ctx)
{
    s_shotPath = getenv("BKR_UI_SHOT_OVERLAY");
    if (Tas_Active() && !s_shotPath) return;   // scripted runs drive the game; no overlay
    if (s_shotPath)
    {
        const char* after = getenv("BKR_UI_SHOT_AFTER");
        s_shotOpenAt = Platform_TickMs() + (after ? atoi(after) : 30) * 1000ull;
        if (const char* hold = getenv("BKR_UI_SHOT_HOLD")) s_shotHold = atoi(hold) * 1000ull;
    }
    s_dev = device;
    s_ctx = ctx;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // nothing to remember between runs
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    std::string font = Platform_UiFont();
    if (!font.empty()) io.Fonts->AddFontFromFileTTF(font.c_str(), 18.0f);
    SettingsUI_AddFallbackFonts();
    SettingsUI_ApplyStyle();
    UiBackend_Init(window);
    ImGui_ImplDX11_Init(device, ctx);
    UiImage_SetDevice(device);
    s_ready = true;
}

bool Overlay_Ready() { return s_ready; }

void Overlay_Toggle()
{
    if (!s_ready) return;
    s_open = !s_open;
    LOG_INFO("settings overlay {}", s_open ? "opened" : "closed");
}

bool Overlay_IsOpen() { return s_open; }

bool Overlay_TakeHomePress() { return s_homePress.exchange(false); }

bool Overlay_ShotPending() { return s_shotPath && s_shotOpenAt && Platform_TickMs() >= s_shotOpenAt; }

void Overlay_Render(float w, float h)
{
    if (!s_ready) return;
    UiBackend_ReplayInput();
    if (s_shotPath && s_shotOpenAt && Platform_TickMs() >= s_shotOpenAt && !s_open) s_open = true;
    if (!s_open) return;

    ImGui_ImplDX11_NewFrame();
    UiBackend_NewFrame();
    ImGui::GetIO().DisplaySize = ImVec2(w, h);   // the bound target (an off-screen one for screenshots)
    // scale with the window: designed for 720p
    float scale = std::max(0.75f, ImGui::GetIO().DisplaySize.y / 720.0f);
    ImGui::GetStyle().FontScaleMain = scale;
    ImGui::NewFrame();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 size(std::min(vp->Size.x * 0.9f, 900.0f * scale), std::min(vp->Size.y * 0.88f, 630.0f * scale));
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    bool open = true;
    if (ImGui::Begin("Sonic and the Black Knight###overlay", &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                          ImGuiWindowFlags_NoSavedSettings))
    {
        float bottom = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        ImGui::BeginChild("panel", ImVec2(0, -bottom), ImGuiChildFlags_NavFlattened);
        if (ImGui::BeginTabBar("overlayTabs"))
        {
            static int shotFrames = 0;
            bool shotMods = SettingsUI_ShotMods() && shotFrames++ < 2;
            static int selected = 0;
            int pick = SettingsUI_PadTab(selected, 2, true);
            if (SettingsUI_TabItem("Settings", 0, pick))
            {
                selected = 0;
                SettingsUI_Draw(SettingsHost::InGame);
                ImGui::EndTabItem();
            }
            if (SettingsUI_TabItem("Mods", 1, pick, shotMods))
            {
                selected = 1;
                SettingsUI_DrawMods(SettingsHost::InGame);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
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
            Platform_Terminate(0);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("View / F1 / Esc closes");
        SettingsUI_DrawVersion();
    }
    ImGui::End();
    if (!open) s_open = false;
    if (SettingsUI_TakeRestartRequest()) App_Restart();

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (s_shotPath && s_shotOpenAt && Platform_TickMs() >= s_shotOpenAt + s_shotHold)
    {
        UI_SaveBoundTarget(s_dev, s_ctx, s_shotPath);
        LOG_INFO("settings overlay screenshot saved to {}", s_shotPath);
        s_shotOpenAt = 0;
        s_open = false;
    }
}
