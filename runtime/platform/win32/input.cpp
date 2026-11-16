// Controllers, keyboard and mouse on Windows. Xbox controllers are read with XInput;
// PlayStation, Switch Pro and other controllers through SDL3's gamepad API (HIDAPI
// drivers for USB and Bluetooth, DirectInput for the rest). SDL's own XInput driver stays
// on so that DirectInput skips XInput devices, but the controllers it reports are
// ignored: no controller is seen twice.
#include "../input.h"
#include "../sdl_gamepad.h"
#include "../../input/gamepad.h"
#include "../../log.h"

#include <cstdlib>
#include <imgui.h>
#include <mutex>
#include <vector>
#include <windows.h>
#include <Xinput.h>

namespace
{
// ---------------------------------------------------------------- XInput
// Querying an empty XInput slot is slow: absent ones are re-checked at most once a second
// (always: `force`).
ULONGLONG s_xLastCheck[4];
bool s_xConnected[4];

bool XInputRead(int i, XINPUT_STATE& st, bool force)
{
    ULONGLONG now = GetTickCount64();
    if (!force && !s_xConnected[i] && s_xLastCheck[i] && now - s_xLastCheck[i] < 1000) return false;
    s_xConnected[i] = XInputGetState(i, &st) == ERROR_SUCCESS;
    s_xLastCheck[i] = now;
    return s_xConnected[i];
}

bool XInputPresent(int i)
{
    XINPUT_STATE st{};
    return XInputRead(i, st, false);
}

// ---------------------------------------------------------------- SDL
// Player slots: an XInput controller keeps its XInput number, the others take the free
// slots (and move when an XInput controller is plugged in with their number).
std::recursive_mutex s_mutex;
bool s_init = false, s_failed = false;
SDL_Gamepad* s_slot[4];
std::vector<SDL_Gamepad*> s_waiting;   // connected, no free slot (four controllers already)
ULONGLONG s_lastUpdate = 0, s_lastScan = 0;

bool Init()
{
    if (s_init) return true;
    if (s_failed) return false;
    // scripted and screenshot runs use no real controller (and must not touch it: SDL
    // sets the light bar and player LEDs when it opens a PlayStation controller)
    if (Gamepad_Scripted() || getenv("BKR_UI_SHOT") || getenv("BKR_UI_SHOT_OVERLAY"))
    {
        s_failed = true;
        return false;
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Xbox controllers belong to XInput: keep them away from SDL's other drivers
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD))
    {
        LOG_WARN("SDL gamepad support unavailable: {}", SDL_GetError());
        s_failed = true;
        return false;
    }
    // polled with SDL_UpdateGamepads; no event queue to drain
    SDL_SetGamepadEventsEnabled(false);
    SDL_SetJoystickEventsEnabled(false);
    s_init = true;
    return true;
}

bool IsXInput(SDL_JoystickID id)
{
    SDL_GUID g = SDL_GetGamepadGUIDForID(id);
    return g.data[14] == 'x';   // SDL's XInput driver
}

bool IsOpen(SDL_JoystickID id)
{
    for (SDL_Gamepad* p : s_slot)
        if (p && SDL_GetGamepadID(p) == id) return true;
    for (SDL_Gamepad* p : s_waiting)
        if (SDL_GetGamepadID(p) == id) return true;
    return false;
}

const char* Name(SDL_Gamepad* p)
{
    const char* n = SDL_GetGamepadName(p);
    return n ? n : "?";
}

void Close(SDL_Gamepad* p, int slot)
{
    if (slot >= 0) LOG_INFO("controller {} disconnected: {}", slot + 1, Name(p));
    SDL_CloseGamepad(p);
}

void Scan()
{
    bool xinput[4];
    for (int i = 0; i < 4; i++) xinput[i] = XInputPresent(i);
    // gone
    for (int i = 0; i < 4; i++)
        if (s_slot[i] && !SDL_GamepadConnected(s_slot[i]))
        {
            Close(s_slot[i], i);
            s_slot[i] = nullptr;
        }
    for (size_t i = 0; i < s_waiting.size();)
        if (!SDL_GamepadConnected(s_waiting[i]))
        {
            Close(s_waiting[i], -1);
            s_waiting.erase(s_waiting.begin() + i);
        }
        else
            i++;
    // new
    int n = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&n))
    {
        for (int i = 0; i < n; i++)
            if (!IsXInput(ids[i]) && !IsOpen(ids[i]))
                if (SDL_Gamepad* p = SDL_OpenGamepad(ids[i])) s_waiting.push_back(p);
        SDL_free(ids);
    }
    // an XInput controller takes back its slot from an SDL one
    for (int i = 0; i < 4; i++)
        if (s_slot[i] && xinput[i])
        {
            s_waiting.insert(s_waiting.begin(), s_slot[i]);
            s_slot[i] = nullptr;
        }
    // free slots, lowest first
    for (int i = 0; i < 4 && !s_waiting.empty(); i++)
        if (!s_slot[i] && !xinput[i])
        {
            s_slot[i] = s_waiting.front();
            s_waiting.erase(s_waiting.begin());
            SDL_SetGamepadPlayerIndex(s_slot[i], i);   // player LEDs on PlayStation controllers
            LOG_INFO("controller {} connected: {} (SDL)", i + 1, Name(s_slot[i]));
        }
}

// Polls SDL's controllers (at most every 2 ms) and looks for new and removed ones (every
// 250 ms). Called with s_mutex held; false when SDL is not used.
bool Update()
{
    if (!Init()) return false;
    ULONGLONG now = GetTickCount64();
    if (now - s_lastUpdate >= 2)
    {
        SDL_UpdateGamepads();
        s_lastUpdate = now;
    }
    if (now - s_lastScan >= 250)
    {
        Scan();
        s_lastScan = now;
    }
    return true;
}
}   // namespace

bool Pad_Read(int index, PadState& out)
{
    if (index < 0 || index > 3) return false;
    // XInput first, as before SDL was added (player 1's slot is always queried)
    XINPUT_STATE st{};
    if (XInputRead(index, st, index == 0))
    {
        out.buttons = st.Gamepad.wButtons;
        out.lx = st.Gamepad.sThumbLX;
        out.ly = st.Gamepad.sThumbLY;
        out.rx = st.Gamepad.sThumbRX;
        out.ry = st.Gamepad.sThumbRY;
        out.lt = st.Gamepad.bLeftTrigger;
        out.rt = st.Gamepad.bRightTrigger;
        return true;
    }
    std::lock_guard lk(s_mutex);
    if (!Update() || !s_slot[index]) return false;
    SdlGamepad_Read(s_slot[index], out);
    return true;
}

void Pad_SetRumble(int index, uint16_t low, uint16_t high)
{
    if (index < 0 || index > 3) return;
    XINPUT_VIBRATION v{};
    v.wLeftMotorSpeed = low;
    v.wRightMotorSpeed = high;
    XInputSetState(index, &v);   // harmless when no XInput controller has this number
    if (s_xConnected[index]) return;
    // stopping also happens in the crash handler: never wait for the lock there
    std::unique_lock lk(s_mutex, std::defer_lock);
    if (low || high) lk.lock();
    else if (!lk.try_lock()) return;
    // XInput motors run until changed; SDL's stop after a duration (at most ~65 s)
    if (s_init && s_slot[index]) SDL_RumbleGamepad(s_slot[index], low, high, low || high ? 0xFFFF : 0);
}

bool Pad_PlayStation(int index)
{
    if (index < 0 || index > 3 || s_xConnected[index]) return false;
    std::lock_guard lk(s_mutex);
    return s_init && s_slot[index] && SdlGamepad_PlayStation(s_slot[index]);
}

// ImGui's Win32 backend reads XInput user 0 only: without one, the launcher and the
// settings are driven by the first SDL controller (ui.cpp, every frame).
void Pad_FeedImGui()
{
    if (XInputPresent(0)) return;   // two sources would fight
    std::lock_guard lk(s_mutex);
    if (!Update()) return;
    SDL_Gamepad* p = nullptr;
    for (SDL_Gamepad* s : s_slot)
        if (s && !p) p = s;
    if (!p && !s_waiting.empty()) p = s_waiting.front();
    if (!p) return;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    static const struct { ImGuiKey k; SDL_GamepadButton b; } kButtons[] = {
        { ImGuiKey_GamepadStart, SDL_GAMEPAD_BUTTON_START }, { ImGuiKey_GamepadBack, SDL_GAMEPAD_BUTTON_BACK },
        { ImGuiKey_GamepadFaceDown, SDL_GAMEPAD_BUTTON_SOUTH }, { ImGuiKey_GamepadFaceRight, SDL_GAMEPAD_BUTTON_EAST },
        { ImGuiKey_GamepadFaceLeft, SDL_GAMEPAD_BUTTON_WEST }, { ImGuiKey_GamepadFaceUp, SDL_GAMEPAD_BUTTON_NORTH },
        { ImGuiKey_GamepadDpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT }, { ImGuiKey_GamepadDpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
        { ImGuiKey_GamepadDpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP }, { ImGuiKey_GamepadDpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN },
        { ImGuiKey_GamepadL1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER }, { ImGuiKey_GamepadR1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
        { ImGuiKey_GamepadL3, SDL_GAMEPAD_BUTTON_LEFT_STICK }, { ImGuiKey_GamepadR3, SDL_GAMEPAD_BUTTON_RIGHT_STICK },
    };
    for (auto& k : kButtons) io.AddKeyEvent(k.k, SDL_GetGamepadButton(p, k.b));
    // analog: the same thresholds as the Win32 backend uses for XInput
    auto analog = [&](ImGuiKey key, SDL_GamepadAxis a, float lo, float hi) {
        float v = (SDL_GetGamepadAxis(p, a) - lo) / (hi - lo);
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        io.AddKeyAnalogEvent(key, v > 0.1f, v);
    };
    analog(ImGuiKey_GamepadL2, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 3900, 32767);
    analog(ImGuiKey_GamepadR2, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 3900, 32767);
    // SDL: down and right positive
    analog(ImGuiKey_GamepadLStickLeft, SDL_GAMEPAD_AXIS_LEFTX, -7849, -32768);
    analog(ImGuiKey_GamepadLStickRight, SDL_GAMEPAD_AXIS_LEFTX, 7849, 32767);
    analog(ImGuiKey_GamepadLStickUp, SDL_GAMEPAD_AXIS_LEFTY, -7849, -32768);
    analog(ImGuiKey_GamepadLStickDown, SDL_GAMEPAD_AXIS_LEFTY, 7849, 32767);
    analog(ImGuiKey_GamepadRStickLeft, SDL_GAMEPAD_AXIS_RIGHTX, -7849, -32768);
    analog(ImGuiKey_GamepadRStickRight, SDL_GAMEPAD_AXIS_RIGHTX, 7849, 32767);
    analog(ImGuiKey_GamepadRStickUp, SDL_GAMEPAD_AXIS_RIGHTY, -7849, -32768);
    analog(ImGuiKey_GamepadRStickDown, SDL_GAMEPAD_AXIS_RIGHTY, 7849, 32767);
}

bool Key_Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
