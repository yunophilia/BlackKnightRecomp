// Controllers (XInput), keyboard and mouse on Windows.
#include "../input.h"

#include <windows.h>
#include <Xinput.h>

bool Pad_Read(int index, PadState& out)
{
    XINPUT_STATE st{};
    if (XInputGetState(index, &st) != ERROR_SUCCESS) return false;
    out.buttons = st.Gamepad.wButtons;
    out.lx = st.Gamepad.sThumbLX;
    out.ly = st.Gamepad.sThumbLY;
    out.rx = st.Gamepad.sThumbRX;
    out.ry = st.Gamepad.sThumbRY;
    out.lt = st.Gamepad.bLeftTrigger;
    out.rt = st.Gamepad.bRightTrigger;
    return true;
}

void Pad_SetRumble(int index, uint16_t low, uint16_t high)
{
    XINPUT_VIBRATION v{};
    v.wLeftMotorSpeed = low;
    v.wRightMotorSpeed = high;
    XInputSetState(index, &v);
}

bool Key_Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
