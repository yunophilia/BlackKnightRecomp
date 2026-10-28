#include "settings_ui.h"
#include "../config.h"
#include "../gx/video.h"
#include "../input/gamepad.h"
#include "mods_ui.h"
#include "../version.h"

#include <imgui.h>
#include <windows.h>

#include <algorithm>
#include <format>
#include <string>

extern bool g_asyncShaders;
void Renderer_ResetSamplers();

namespace
{
bool s_restartRequested = false;
// keyboard/mouse rebinding: which binding waits for a key, and whether it adds or replaces
int s_capture = -1;
bool s_captureAdd = false;
bool s_captureArmed = false;   // all keys were released since "Set" was clicked
bool s_askRestart = false;          // open the "needs a restart" prompt next frame
const char* s_help = nullptr;      // description of the hovered setting

// Applies settings that can change while the game runs (called on the render thread).
void ApplyLive()
{
    g_videoConfig.vsync = g_config.vsync;
    g_asyncShaders = g_config.asyncShaders;
    Video_SetFullscreen(g_config.fullscreen);
    Video_SetWindowSize(g_config.windowWidth, g_config.windowHeight);
    Renderer_ResetSamplers();      // anisotropy
    Gamepad_ReloadBindings();
    // rumble, remote speaker and pause-on-focus-loss read g_config directly
}

// A setting changed: save it, then apply it live or (in game) offer a restart.
void Changed(SettingsHost host, bool needsRestart)
{
    Config_Save();
    if (host != SettingsHost::InGame) return;
    if (needsRestart) s_askRestart = true;
    else ApplyLive();
}

void Help(const char* text)
{
    if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) s_help = text;
}

const char* RestartTag(SettingsHost host, bool needsRestart)
{
    return host == SettingsHost::InGame && needsRestart ? " (restart)" : "";
}

bool Check(SettingsHost host, const char* label, bool& v, bool needsRestart, const char* help)
{
    std::string l = std::string(label) + RestartTag(host, needsRestart);
    bool changed = ImGui::Checkbox(l.c_str(), &v);
    Help(help);
    if (changed) Changed(host, needsRestart);
    return changed;
}

template <typename T>
bool Combo(SettingsHost host, const char* label, T& value, const T* values, const char* const* names, int count,
           bool needsRestart, const char* help)
{
    int cur = 0;
    for (int i = 0; i < count; i++)
        if (values[i] == value) cur = i;
    std::string l = std::string(label) + RestartTag(host, needsRestart);
    bool changed = false;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::BeginCombo(l.c_str(), names[cur]))
    {
        for (int i = 0; i < count; i++)
            if (ImGui::Selectable(names[i], i == cur) && i != cur)
            {
                value = values[i];
                changed = true;
            }
        ImGui::EndCombo();
    }
    Help(help);
    return changed;   // the caller saves (Changed) once the new value is in g_config
}

void VideoTab(SettingsHost host)
{
    Config& c = g_config;
    ImGui::SeparatorText("Display");
    Check(host, "Fullscreen", c.fullscreen, false, "Borderless fullscreen. F11 or Alt+Enter also toggle it.");
    Check(host, "VSync", c.vsync, false, "Wait for the monitor's refresh. The game paces itself, so this is only needed against tearing.");
    Check(host, "Widescreen (16:9)", c.widescreen, true, "16:9 like the Wii's widescreen setting. Off: 4:3.");
    static const int sizes[][2] = { { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
    int sizeIdx = 0;
    const int sizeIds[] = { 0, 1, 2, 3, 4 };
    const char* sizeNames[] = { "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160" };
    for (int i = 0; i < 5; i++)
        if (sizes[i][0] == c.windowWidth && sizes[i][1] == c.windowHeight) sizeIdx = i;
    if (Combo(host, "Window size", sizeIdx, sizeIds, sizeNames, 5, false, "Size of the window when not fullscreen."))
    {
        c.windowWidth = sizes[sizeIdx][0];
        c.windowHeight = sizes[sizeIdx][1];
        Changed(host, false);
    }

    ImGui::SeparatorText("Rendering");
    const int fps[] = { 30, 60 };
    const char* fpsNames[] = { "30 (original)", "60" };
    if (Combo(host, "Frame rate", c.frameRate, fps, fpsNames, 2, true, "60 doubles the frame rate. Game speed stays the same."))
        Changed(host, true);
    const int scales[] = { 1, 2, 3, 4, 5, 6 };
    const char* scaleNames[] = { "1x (640 x 528)", "2x (1280 x 1056)", "3x (1920 x 1584)", "4x (2560 x 2112)", "5x (3200 x 2640)", "6x (3840 x 3168)" };
    if (Combo(host, "Internal resolution", c.internalScale, scales, scaleNames, 6, true, "Resolution the game is rendered at."))
        Changed(host, true);
    const int anis[] = { 1, 2, 4, 8, 16 };
    const char* anisNames[] = { "Off (as the game sets it)", "2x", "4x", "8x", "16x" };
    if (Combo(host, "Anisotropic filtering", c.anisotropy, anis, anisNames, 5, false, "Sharper textures at grazing angles."))
        Changed(host, false);
    Check(host, "Compile shaders in the background", c.asyncShaders, false,
          "Avoids hitches when new effects appear; an object may then show up a frame late.");
    Check(host, "Graphics on a second CPU core", c.gpuThread, true, "Like Dolphin's dual core mode. Leave on unless debugging.");
    Check(host, "HD texture packs", c.texturePack, true,
          "Dolphin-format packs in textures/<game ID> (e.g. RENE8P) next to the game, game/textures or Dolphin's Load/Textures.");
}

void InputTab(SettingsHost host)
{
    Config& c = g_config;
    ImGui::SeparatorText("Controller");
    Check(host, "Rumble", c.rumble, false, "Vibrate the controller where the Wii Remote would rumble.");
    ImGui::TextDisabled("View (Back) opens these settings. View + Start is the Wii HOME button.");

    ImGui::SeparatorText("Button bindings");
    static const char* actions[12] = { "Jump / homing attack (Wii A)", "Slash (swing the Remote)", "Soul Surge (Wii B)",
                                       "Shake Nunchuk", "Guard / talk (Z)", "Nunchuk C", "Pause (Wii +)",
                                       "Wii - (unused by the game)", "Wii 1", "Wii 2", "Thrust Remote forward",
                                       "Pull Remote back" };
    static const std::string buttons[] = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "START", "BACK",
                                           "LS", "RS", "UP", "DOWN", "LEFT", "RIGHT", "NONE" };
    static const char* names[] = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Start", "View (Back, also opens settings)",
                                   "Left stick click", "Right stick click", "D-pad up", "D-pad down", "D-pad left",
                                   "D-pad right", "None" };
    for (int i = 0; i < 12; i++)
    {
        ImGui::PushID(i);
        std::string v = c.bind[i];
        for (char& ch : v) ch = (char)toupper((unsigned char)ch);
        if (Combo(host, actions[i], v, buttons, names, 17, false, "Controller button for this Wii action."))
        {
            c.bind[i] = v;
            Changed(host, false);
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Restore default bindings"))
    {
        Config defaults;
        for (int i = 0; i < 12; i++) c.bind[i] = defaults.bind[i];
        Changed(host, false);
    }

    ImGui::SeparatorText("Keyboard and mouse");
    static const char* keyActions[21] = { "Jump / homing attack (Wii A)", "Slash (swing the Remote)", "Soul Surge (Wii B)",
                                          "Shake Nunchuk", "Guard / talk (Z)", "Nunchuk C", "Pause (Wii +)", "Wii -",
                                          "Wii 1", "Wii 2", "Thrust Remote forward", "Pull Remote back", "Wii HOME button",
                                          "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Move forward",
                                          "Move back", "Move left", "Move right" };
    if (s_capture >= 0)
    {
        // waiting for a key: wait until everything is released (the click on "Set"), then
        // take the next key or mouse button; Escape cancels
        int held = Gamepad_HeldKey();
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) s_capture = -1;
        else if (!s_captureArmed) s_captureArmed = held == 0;
        else if (held)
        {
            std::string name = Gamepad_KeyName(held);
            std::string& cur = c.keys[s_capture];
            if (s_captureAdd && _stricmp(cur.c_str(), "NONE") != 0 && !cur.empty())
            {
                bool dup = false;
                for (int vk : Gamepad_ParseKeys(cur)) dup |= vk == held;
                if (!dup) cur += ", " + name;
            }
            else
                cur = name;
            s_capture = -1;
            Changed(host, false);
        }
    }
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_SizingStretchProp))
    {
        for (int i = 0; i < 21; i++)
        {
            ImGui::PushID(100 + i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(keyActions[i]);
            ImGui::TableNextColumn();
            if (s_capture == i)
                ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_CheckMark], "Press a key or mouse button (Esc cancels)");
            else
            {
                if (ImGui::Button("Set")) { s_capture = i; s_captureAdd = false; s_captureArmed = false; }
                Help("Replace this binding: click, then press a key or mouse button.");
                ImGui::SameLine();
                if (ImGui::Button("+")) { s_capture = i; s_captureAdd = true; s_captureArmed = false; }
                Help("Add another key or mouse button for this action.");
                ImGui::SameLine();
                if (ImGui::Button("Clear")) { c.keys[i] = "NONE"; Changed(host, false); }
                ImGui::SameLine();
                ImGui::TextUnformatted(c.keys[i].c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Restore default keyboard and mouse"))
    {
        Config defaults;
        for (int i = 0; i < 21; i++) c.keys[i] = defaults.keys[i];
        Changed(host, false);
    }
    ImGui::TextDisabled("F1 opens the settings, F11 toggles fullscreen; the mouse also moves the Wii Remote pointer.");
}

void AudioTab(SettingsHost host)
{
    ImGui::SeparatorText("Audio");
    Check(host, "Wii Remote speaker sounds", g_config.remoteSpeaker, false,
          "Play the sounds the game sends to the Wii Remote speaker through the PC.");
}

void SystemTab(SettingsHost host)
{
    Config& c = g_config;
    ImGui::SeparatorText("System");
    const int langs[] = { 1, 2, 3, 4, 5, 6, 0 };
    const char* langNames[] = { "English", "German", "French", "Spanish", "Italian", "Dutch", "Japanese" };
    if (Combo(host, "Language", c.language, langs, langNames, 7, true, "Language of the game's text and menus (the Wii system language)."))
        Changed(host, true);
    Check(host, "Pause when the window is in the background", c.pauseOnFocusLoss, false,
          "Freeze the game while another window has focus.");
    ImGui::Spacing();
    ImGui::TextDisabled("Settings file: %s", Config_Path().c_str());
}

void ModsTab(SettingsHost host)
{
    const char* help = nullptr;
    if (ModsUI_Draw(host, help)) Changed(host, true);
    if (help) s_help = help;
}
}   // namespace

void SettingsUI_SetRelease(const std::string& gameId, const std::string& dataDir) { ModsUI_SetRelease(gameId, dataDir); }
std::vector<std::string> SettingsUI_ActiveMods() { return ModsUI_ActiveMods(); }

namespace
{
// A page with the description of the hovered item below it, and the restart question
void Page(SettingsHost host, void (*draw)(SettingsHost), float helpH)
{
    ImGui::BeginChild("page", ImVec2(0, -helpH), ImGuiChildFlags_None);
    draw(host);
    static const bool shotBottom = getenv("BKR_UI_SHOT_BOTTOM") != nullptr;   // test aid
    if (shotBottom) ImGui::SetScrollY(ImGui::GetScrollMaxY());
    ImGui::EndChild();
}

void Footer()
{
    ImGui::Separator();
    ImGui::TextWrapped("%s", s_help ? s_help : "");

    if (s_askRestart)
    {
        ImGui::OpenPopup("Restart needed");
        s_askRestart = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Restart needed", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("This setting needs a restart. Restart the game?");
        ImGui::TextDisabled("Your progress is saved whenever the game saves; unsaved progress is lost.");
        ImGui::Spacing();
        if (ImGui::Button("Restart now"))
        {
            s_restartRequested = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Later")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

float HelpHeight() { return ImGui::GetTextLineHeightWithSpacing() * 3; }
}   // namespace

void SettingsUI_Draw(SettingsHost host)
{
    s_help = nullptr;
    if (ImGui::BeginTabBar("settings"))
    {
        struct Tab { const char* name; void (*draw)(SettingsHost); };
        static const Tab tabs[] = { { "Video", VideoTab }, { "Input", InputTab }, { "Audio", AudioTab }, { "System", SystemTab } };
        // test aid: BKR_UI_SHOT_SUBTAB=<tab name> opens that tab first (UI screenshots)
        static const char* shotTab = getenv("BKR_UI_SHOT_SUBTAB");
        static int shotFrames = 0;
        bool pick = shotTab && shotFrames++ < 2;
        for (auto& t : tabs)
            if (ImGui::BeginTabItem(t.name, nullptr, pick && !_stricmp(shotTab, t.name) ? ImGuiTabItemFlags_SetSelected : 0))
            {
                Page(host, t.draw, HelpHeight());
                ImGui::EndTabItem();
            }
        ImGui::EndTabBar();
    }
    Footer();
}

void SettingsUI_DrawMods(SettingsHost host)
{
    s_help = nullptr;
    Page(host, ModsTab, HelpHeight());
    Footer();
}

bool SettingsUI_ShotMods()
{
    const char* tab = getenv("BKR_UI_SHOT_SUBTAB");
    return tab && !_stricmp(tab, "Mods");
}

bool SettingsUI_TakeRestartRequest()
{
    bool r = s_restartRequested;
    s_restartRequested = false;
    return r;
}

void SettingsUI_DrawVersion()
{
    ImGui::SameLine();
    float pad = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(g_version).x;
    if (pad > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    ImGui::TextDisabled("%s", g_version);
}

void SettingsUI_AddFallbackFonts()
{
    // after the main font: Chinese, Japanese and Korean names and symbols (mod titles) from
    // Windows' fonts where they exist; glyphs load as text needs them
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
    char dir[MAX_PATH];
    UINT n = GetWindowsDirectoryA(dir, MAX_PATH);
    if (!n) return;
    ImFontConfig cfg;
    cfg.MergeMode = true;
    for (const char* file : { "msyh.ttc", "msjh.ttc", "YuGothM.ttc", "malgun.ttf", "seguisym.ttf" })
    {
        std::string path = std::string(dir, n) + "\\Fonts\\" + file;
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) io.Fonts->AddFontFromFileTTF(path.c_str(), 18.0f, &cfg);
    }
}

void SettingsUI_ApplyStyle()
{
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 6;
    s.FrameRounding = 4;
    s.GrabRounding = 4;
    s.TabRounding = 4;
    s.WindowPadding = ImVec2(14, 12);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 8);
    ImVec4* c = s.Colors;
    const ImVec4 blue(0.12f, 0.44f, 0.92f, 1.00f), blueLight(0.23f, 0.51f, 0.96f, 1.00f),
        blueDim(0.10f, 0.30f, 0.60f, 1.00f);
    c[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.067f, 0.086f, 0.94f);
    c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.08f, 0.10f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.24f, 0.30f, 0.40f, 0.60f);
    c[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.13f, 0.17f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.14f, 0.20f, 0.30f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.25f, 0.40f, 1.00f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = ImVec4(0.07f, 0.08f, 0.11f, 1.00f);
    c[ImGuiCol_CheckMark] = blue;
    c[ImGuiCol_SliderGrab] = blue;
    c[ImGuiCol_SliderGrabActive] = blueLight;
    c[ImGuiCol_Button] = ImVec4(0.13f, 0.17f, 0.24f, 1.00f);
    c[ImGuiCol_ButtonHovered] = blueLight;
    c[ImGuiCol_ButtonActive] = blue;
    c[ImGuiCol_Header] = blueDim;
    c[ImGuiCol_HeaderHovered] = blueLight;
    c[ImGuiCol_HeaderActive] = blue;
    c[ImGuiCol_Tab] = ImVec4(0.09f, 0.11f, 0.15f, 1.00f);
    c[ImGuiCol_TabHovered] = blueLight;
    c[ImGuiCol_TabSelected] = blue;
    c[ImGuiCol_TabSelectedOverline] = blueLight;
    c[ImGuiCol_SeparatorHovered] = blueLight;
    c[ImGuiCol_Text] = ImVec4(0.90f, 0.93f, 0.95f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.58f, 0.62f, 1.00f);
    c[ImGuiCol_NavCursor] = blue;
    c[ImGuiCol_PlotHistogram] = blue;
}
