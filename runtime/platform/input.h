#pragma once
// Controllers, keyboard and mouse. Controllers are reported in XInput's layout and
// ranges whatever the platform uses (Linux: SDL3 gamepads, which include Steam Input's).
#include <cstdint>

#include "keys.h"

struct PadState
{
    uint16_t buttons = 0;          // PAD_* bits (input/gamepad.h), XInput's values
    int16_t lx = 0, ly = 0;        // sticks, -32768..32767, up is positive
    int16_t rx = 0, ry = 0;
    uint8_t lt = 0, rt = 0;        // triggers 0..255
};

// XInput's suggested dead zones (sticks)
constexpr int16_t PAD_LEFT_THUMB_DEADZONE = 7849;
constexpr int16_t PAD_RIGHT_THUMB_DEADZONE = 8689;

// Controller in player slot 0-3 (false: none).
bool Pad_Read(int index, PadState& out);
// Rumble motors (0 = off .. 65535).
void Pad_SetRumble(int index, uint16_t low, uint16_t high);
// A keyboard key or mouse button (Windows virtual-key code, see keys.h) is held now.
bool Key_Down(int vk);
