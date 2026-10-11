// XInput controllers with a keyboard fallback for player 1.
#include "gamepad.h"

#include <cmath>
#include <windows.h>
#include <Xinput.h>

extern void* Video_HWND();

static float Stick(SHORT v, SHORT dead)
{
    float f = (float)v;
    float d = (float)dead;
    if (std::fabs(f) < d) return 0.0f;
    float s = f > 0 ? 1.0f : -1.0f;
    return s * std::fmin(1.0f, (std::fabs(f) - d) / (32767.0f - d));
}

static bool Key(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool g_autoPress = false;   // test aid: tap A every few seconds

void Gamepad_Poll(int index, GamepadState& out)
{
    out = GamepadState();
    if (g_autoPress && index == 0)
    {
        DWORD t = GetTickCount() % 3000;
        if (t < 150) out.buttons |= PAD_A;
        out.connected = true;
    }
    XINPUT_STATE st{};
    if (XInputGetState(index, &st) == ERROR_SUCCESS)
    {
        out.connected = true;
        out.buttons |= st.Gamepad.wButtons;
        out.lx = Stick(st.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        out.ly = Stick(st.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        out.rx = Stick(st.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        out.ry = Stick(st.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        out.lt = st.Gamepad.bLeftTrigger / 255.0f;
        out.rt = st.Gamepad.bRightTrigger / 255.0f;
    }
    if (index != 0)
        return;
    // keyboard (only when our window has focus)
    if (GetForegroundWindow() != (HWND)Video_HWND())
        return;
    bool any = false;
    auto map = [&](int vk, uint16_t bit) { if (Key(vk)) { out.buttons |= bit; any = true; } };
    map(VK_SPACE, PAD_A);      // jump
    map('J', PAD_X);           // slash
    map('K', PAD_B);
    map('L', PAD_Y);
    map(VK_LSHIFT, PAD_RB);    // guard
    map('Q', PAD_LB);
    map(VK_RETURN, PAD_START);
    map(VK_BACK, PAD_BACK);
    map(VK_UP, PAD_DPAD_UP);
    map(VK_DOWN, PAD_DPAD_DOWN);
    map(VK_LEFT, PAD_DPAD_LEFT);
    map(VK_RIGHT, PAD_DPAD_RIGHT);
    float kx = (Key('D') ? 1.0f : 0.0f) - (Key('A') ? 1.0f : 0.0f);
    float ky = (Key('W') ? 1.0f : 0.0f) - (Key('S') ? 1.0f : 0.0f);
    if (kx || ky)
    {
        out.lx = kx;
        out.ly = ky;
        any = true;
    }
    if (Key('U')) { out.rt = 1.0f; any = true; }
    if (any) out.connected = true;
}
