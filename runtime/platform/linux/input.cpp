// Controllers (SDL3 gamepads, Steam Input's virtual pads included), keyboard and mouse
// on Linux, translated to XInput's layout and Windows key codes.
#include "../input.h"
#include "sdl.h"
#include "../../input/gamepad.h"
#include "../../log.h"

#include <mutex>

namespace
{
std::mutex s_padMutex;
SDL_Gamepad* s_pads[4];   // player slots, in the order the controllers were connected

void AddPad(SDL_JoystickID id)
{
    std::lock_guard lk(s_padMutex);
    for (SDL_Gamepad* p : s_pads)
        if (p && SDL_GetGamepadID(p) == id) return;
    for (int i = 0; i < 4; i++)
        if (!s_pads[i])
        {
            s_pads[i] = SDL_OpenGamepad(id);
            if (s_pads[i]) LOG_INFO("controller {} connected: {}", i + 1, SDL_GetGamepadName(s_pads[i]) ? SDL_GetGamepadName(s_pads[i]) : "?");
            return;
        }
}

void RemovePad(SDL_JoystickID id)
{
    std::lock_guard lk(s_padMutex);
    for (int i = 0; i < 4; i++)
        if (s_pads[i] && SDL_GetGamepadID(s_pads[i]) == id)
        {
            LOG_INFO("controller {} disconnected", i + 1);
            SDL_CloseGamepad(s_pads[i]);
            s_pads[i] = nullptr;
        }
}

int16_t Axis(SDL_Gamepad* p, SDL_GamepadAxis a, bool invert)
{
    int v = SDL_GetGamepadAxis(p, a);
    if (invert) v = -v - 1;   // SDL: down is positive; XInput: up is positive
    return (int16_t)v;
}
}   // namespace

void Input_HandleEvent(const SDL_Event& e)
{
    if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
    else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(e.gdevice.which);
}

bool Pad_Read(int index, PadState& out)
{
    std::lock_guard lk(s_padMutex);
    SDL_Gamepad* p = index >= 0 && index < 4 ? s_pads[index] : nullptr;
    if (!p) return false;
    static const struct { SDL_GamepadButton b; uint16_t bit; } kButtons[] = {
        { SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_DPAD_UP }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_DPAD_DOWN },
        { SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_DPAD_LEFT }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_DPAD_RIGHT },
        { SDL_GAMEPAD_BUTTON_START, PAD_START }, { SDL_GAMEPAD_BUTTON_BACK, PAD_BACK },
        { SDL_GAMEPAD_BUTTON_LEFT_STICK, PAD_LTHUMB }, { SDL_GAMEPAD_BUTTON_RIGHT_STICK, PAD_RTHUMB },
        { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_LB }, { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_RB },
        // by position, like XInput: bottom A, right B, left X, top Y
        { SDL_GAMEPAD_BUTTON_SOUTH, PAD_A }, { SDL_GAMEPAD_BUTTON_EAST, PAD_B },
        { SDL_GAMEPAD_BUTTON_WEST, PAD_X }, { SDL_GAMEPAD_BUTTON_NORTH, PAD_Y },
    };
    out = PadState();
    for (auto& k : kButtons)
        if (SDL_GetGamepadButton(p, k.b)) out.buttons |= k.bit;
    out.lx = Axis(p, SDL_GAMEPAD_AXIS_LEFTX, false);
    out.ly = Axis(p, SDL_GAMEPAD_AXIS_LEFTY, true);
    out.rx = Axis(p, SDL_GAMEPAD_AXIS_RIGHTX, false);
    out.ry = Axis(p, SDL_GAMEPAD_AXIS_RIGHTY, true);
    out.lt = (uint8_t)(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) * 255 / 32767);
    out.rt = (uint8_t)(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * 255 / 32767);
    return true;
}

void Pad_SetRumble(int index, uint16_t low, uint16_t high)
{
    std::lock_guard lk(s_padMutex);
    SDL_Gamepad* p = index >= 0 && index < 4 ? s_pads[index] : nullptr;
    // XInput motors run until changed; SDL's stop after a duration (at most ~65 s)
    if (p) SDL_RumbleGamepad(p, low, high, low || high ? 0xFFFF : 0);
}

bool Key_Down(int vk)
{
    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2)
    {
        SDL_MouseButtonFlags b = SDL_GetMouseState(nullptr, nullptr);
        switch (vk)
        {
        case VK_LBUTTON: return b & SDL_BUTTON_LMASK;
        case VK_RBUTTON: return b & SDL_BUTTON_RMASK;
        case VK_MBUTTON: return b & SDL_BUTTON_MMASK;
        case VK_XBUTTON1: return b & SDL_BUTTON_X1MASK;
        default: return b & SDL_BUTTON_X2MASK;
        }
    }
    SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
    if (vk >= 'A' && vk <= 'Z') sc = (SDL_Scancode)(SDL_SCANCODE_A + (vk - 'A'));
    else if (vk >= '1' && vk <= '9') sc = (SDL_Scancode)(SDL_SCANCODE_1 + (vk - '1'));
    else if (vk == '0') sc = SDL_SCANCODE_0;
    else if (vk >= VK_F1 && vk <= VK_F12) sc = (SDL_Scancode)(SDL_SCANCODE_F1 + (vk - VK_F1));
    else if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9) sc = (SDL_Scancode)(SDL_SCANCODE_KP_1 + (vk - VK_NUMPAD1));
    else
        switch (vk)
        {
        case VK_NUMPAD0: sc = SDL_SCANCODE_KP_0; break;
        case VK_SPACE: sc = SDL_SCANCODE_SPACE; break;
        case VK_RETURN: sc = SDL_SCANCODE_RETURN; break;
        case VK_BACK: sc = SDL_SCANCODE_BACKSPACE; break;
        case VK_TAB: sc = SDL_SCANCODE_TAB; break;
        case VK_ESCAPE: sc = SDL_SCANCODE_ESCAPE; break;
        case VK_LSHIFT: sc = SDL_SCANCODE_LSHIFT; break;
        case VK_RSHIFT: sc = SDL_SCANCODE_RSHIFT; break;
        case VK_LCONTROL: sc = SDL_SCANCODE_LCTRL; break;
        case VK_RCONTROL: sc = SDL_SCANCODE_RCTRL; break;
        case VK_LMENU: sc = SDL_SCANCODE_LALT; break;
        case VK_RMENU: sc = SDL_SCANCODE_RALT; break;
        case VK_UP: sc = SDL_SCANCODE_UP; break;
        case VK_DOWN: sc = SDL_SCANCODE_DOWN; break;
        case VK_LEFT: sc = SDL_SCANCODE_LEFT; break;
        case VK_RIGHT: sc = SDL_SCANCODE_RIGHT; break;
        case VK_INSERT: sc = SDL_SCANCODE_INSERT; break;
        case VK_DELETE: sc = SDL_SCANCODE_DELETE; break;
        case VK_HOME: sc = SDL_SCANCODE_HOME; break;
        case VK_END: sc = SDL_SCANCODE_END; break;
        case VK_PRIOR: sc = SDL_SCANCODE_PAGEUP; break;
        case VK_NEXT: sc = SDL_SCANCODE_PAGEDOWN; break;
        }
    if (sc == SDL_SCANCODE_UNKNOWN) return false;
    int n = 0;
    const bool* keys = SDL_GetKeyboardState(&n);
    return keys && sc < n && keys[sc];
}
