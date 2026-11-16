#pragma once
// An SDL3 gamepad read in XInput's layout (platform/input.h), shared by the platforms
// that use SDL for controllers: Linux for all of them, Windows for the non-XInput ones.
#include "input.h"
#include "../input/gamepad.h"

#include <SDL3/SDL.h>

inline void SdlGamepad_Read(SDL_Gamepad* p, PadState& out)
{
    static const struct { SDL_GamepadButton b; uint16_t bit; } kButtons[] = {
        { SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_DPAD_UP }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_DPAD_DOWN },
        { SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_DPAD_LEFT }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_DPAD_RIGHT },
        { SDL_GAMEPAD_BUTTON_START, PAD_START }, { SDL_GAMEPAD_BUTTON_BACK, PAD_BACK },
        { SDL_GAMEPAD_BUTTON_LEFT_STICK, PAD_LTHUMB }, { SDL_GAMEPAD_BUTTON_RIGHT_STICK, PAD_RTHUMB },
        { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_LB }, { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_RB },
        { SDL_GAMEPAD_BUTTON_GUIDE, PAD_HOME },       // PS / Home button: the Wii HOME button
        { SDL_GAMEPAD_BUTTON_TOUCHPAD, PAD_BACK },    // PlayStation touchpad click: like Create (settings)
        // by position, like XInput: bottom A (Cross), right B (Circle), left X (Square), top Y (Triangle)
        { SDL_GAMEPAD_BUTTON_SOUTH, PAD_A }, { SDL_GAMEPAD_BUTTON_EAST, PAD_B },
        { SDL_GAMEPAD_BUTTON_WEST, PAD_X }, { SDL_GAMEPAD_BUTTON_NORTH, PAD_Y },
    };
    auto axis = [p](SDL_GamepadAxis a, bool invert) {
        int v = SDL_GetGamepadAxis(p, a);
        if (invert) v = -v - 1;   // SDL: down is positive; XInput: up is positive
        return (int16_t)v;
    };
    auto trigger = [p](SDL_GamepadAxis a) {
        int v = SDL_GetGamepadAxis(p, a);
        return (uint8_t)(v <= 0 ? 0 : v * 255 / 32767);
    };
    out = PadState();
    for (auto& k : kButtons)
        if (SDL_GetGamepadButton(p, k.b)) out.buttons |= k.bit;
    out.lx = axis(SDL_GAMEPAD_AXIS_LEFTX, false);
    out.ly = axis(SDL_GAMEPAD_AXIS_LEFTY, true);
    out.rx = axis(SDL_GAMEPAD_AXIS_RIGHTX, false);
    out.ry = axis(SDL_GAMEPAD_AXIS_RIGHTY, true);
    out.lt = trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    out.rt = trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
}

inline bool SdlGamepad_PlayStation(SDL_Gamepad* p)
{
    switch (SDL_GetGamepadType(p))
    {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return true;
    default: return false;
    }
}
