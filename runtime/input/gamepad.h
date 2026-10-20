#pragma once
#include <cstdint>

// Host controller state (XInput layout), polled once per guest input read.
struct GamepadState
{
    bool connected = false;
    uint16_t buttons = 0;      // XINPUT_GAMEPAD_* bits
    float lx = 0, ly = 0;      // left stick  -1..1
    float rx = 0, ry = 0;      // right stick -1..1
    float lt = 0, rt = 0;      // triggers 0..1
    float px = -1, py = -1;    // absolute pointer override 0..1 (input scripts), -1 = none
};

enum : uint16_t
{
    PAD_DPAD_UP = 0x0001, PAD_DPAD_DOWN = 0x0002, PAD_DPAD_LEFT = 0x0004, PAD_DPAD_RIGHT = 0x0008,
    PAD_START = 0x0010, PAD_BACK = 0x0020, PAD_LTHUMB = 0x0040, PAD_RTHUMB = 0x0080,
    PAD_LB = 0x0100, PAD_RB = 0x0200, PAD_HOME = 0x0400,   // 0x0400: synthesised (Back+Start or H)
    PAD_A = 0x1000, PAD_B = 0x2000, PAD_X = 0x4000, PAD_Y = 0x8000,
};

void Gamepad_Poll(int index, GamepadState& out);
bool Gamepad_IsConnected(int index);
bool Gamepad_RumbleAllowed();   // a person is playing: no input script, window focused
void Gamepad_StopRumble();      // all motors off
void Gamepad_ReloadBindings();  // config.ini bindings changed (settings UI)
