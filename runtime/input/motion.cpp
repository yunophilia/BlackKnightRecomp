// Gamepad -> Wii Remote/Nunchuk sample synthesis.
//
// Accelerometer convention (raw WPAD axes, counts): at rest, Wii Remote pointing at
// the screen with the buttons up reads (0, 0, +G). Sword swings are produced as
// short acceleration pulses so the game's own motion detection recognises them.
#include "motion.h"
#include "gamepad.h"
#include "../kernel/memory.h"

#include <algorithm>
#include <cmath>
#include <windows.h>

extern void* Video_HWND();

namespace
{
constexpr int G = 100;   // counts per 1 g (matches the reported gravity unit)

enum : uint16_t
{
    WPB_LEFT = 0x0001, WPB_RIGHT = 0x0002, WPB_DOWN = 0x0004, WPB_UP = 0x0008, WPB_PLUS = 0x0010,
    WPB_TWO = 0x0100, WPB_ONE = 0x0200, WPB_B = 0x0400, WPB_A = 0x0800, WPB_MINUS = 0x1000,
    WPB_Z = 0x2000, WPB_C = 0x4000, WPB_HOME = 0x8000,
};

struct Pulse
{
    float t = -1.0f;        // time since start (s), <0 = inactive
    float dur = 0.0f;
    float ax = 0, ay = 0, az = 0;   // peak acceleration (g)
};

struct State
{
    GamepadState pad;
    GamepadState prevPad;
    Pulse remote;
    Pulse nunchuk;
    int lastSlashDir = 1;
    bool rstickArmed = true;
    float rtPrev = 0.0f;
    float xHeld = 0.0f;     // seconds X has been held (auto-repeat slashes)
    float ptrX = 0.5f, ptrY = 0.5f;   // pointer position 0..1 (right stick / mouse)
    POINT lastMouse{ -1, -1 };
    float pitch = 0.0f, roll = 0.0f;   // Remote tilt (radians, forward / right), follows the right stick
};

State s_state[4];

void StartPulse(Pulse& p, float ax, float ay, float az, float dur)
{
    p.t = 0.0f;
    p.dur = dur;
    p.ax = ax;
    p.ay = ay;
    p.az = az;
}

// Smooth out-and-back profile: positive lobe then a smaller negative lobe
float PulseShape(const Pulse& p)
{
    if (p.t < 0.0f || p.t > p.dur) return 0.0f;
    float u = p.t / p.dur;
    if (u < 0.6f) return std::sin(u / 0.6f * 3.14159265f);
    return -0.6f * std::sin((u - 0.6f) / 0.4f * 3.14159265f);
}
}   // namespace

void Motion_Step(int chan, float dt)
{
    State& s = s_state[chan & 3];
    s.prevPad = s.pad;
    Gamepad_Poll(chan, s.pad);
    const GamepadState& p = s.pad;

    // X: slash, alternating direction, or follow the right stick if it is held
    bool xPressed = (p.buttons & PAD_X) && !(s.prevPad.buttons & PAD_X);
    // holding X keeps slashing (one swing every 0.22 s after the first)
    if (p.buttons & PAD_X)
    {
        s.xHeld += dt;
        if (!xPressed && s.xHeld >= 0.22f && s.remote.t < 0.0f)
        {
            s.xHeld = 0.0f;
            xPressed = true;
        }
    }
    else
        s.xHeld = 0.0f;
    float rmag = std::sqrt(p.rx * p.rx + p.ry * p.ry);
    if (xPressed)
    {
        if (rmag > 0.5f)
            StartPulse(s.remote, p.rx * 3.5f / rmag, 0.0f, p.ry * 3.5f / rmag, 0.18f);
        else
        {
            s.lastSlashDir = -s.lastSlashDir;
            StartPulse(s.remote, 3.5f * s.lastSlashDir, 0.0f, 0.0f, 0.18f);
        }
    }
    // Right stick flick: directional slash
    if (rmag > 0.85f && s.rstickArmed)
    {
        s.rstickArmed = false;
        StartPulse(s.remote, p.rx * 3.5f / rmag, 0.0f, p.ry * 3.5f / rmag, 0.18f);
    }
    if (rmag < 0.3f) s.rstickArmed = true;
    // Right trigger: thrust forward; left trigger: pull back
    if (p.rt > 0.6f && s.rtPrev <= 0.6f) StartPulse(s.remote, 0.0f, 3.5f, 0.0f, 0.2f);
    if (p.lt > 0.6f && !(s.prevPad.lt > 0.6f)) StartPulse(s.remote, 0.0f, -3.0f, 0.0f, 0.2f);
    s.rtPrev = p.rt;
    // Y: shake the Nunchuk
    if ((p.buttons & PAD_Y) && !(s.prevPad.buttons & PAD_Y)) StartPulse(s.nunchuk, 3.0f, 0.0f, 3.0f, 0.2f);

    // Right stick held: tilt the Remote (like Dolphin's Tilt input: up to 85 degrees,
    // eased towards the stick so it does not snap); levels out when released.
    // Same directions as the usual Dolphin profile: stick down tilts forward.
    constexpr float kMaxTilt = 85.0f * 3.14159265f / 180.0f;
    float ease = 1.0f - std::exp(-dt / 0.05f);
    s.pitch += (-p.ry * kMaxTilt - s.pitch) * ease;
    s.roll += (p.rx * kMaxTilt - s.roll) * ease;

    // Pointer: the right stick moves it; moving the mouse inside the window takes over.
    s.ptrX = std::clamp(s.ptrX + p.rx * dt * 0.9f, 0.0f, 1.0f);
    s.ptrY = std::clamp(s.ptrY - p.ry * dt * 0.9f, 0.0f, 1.0f);
    if (chan == 0)
    {
        HWND hwnd = (HWND)Video_HWND();
        POINT pt;
        RECT rc;
        if (hwnd && GetCursorPos(&pt) && (pt.x != s.lastMouse.x || pt.y != s.lastMouse.y))
        {
            s.lastMouse = pt;
            if (ScreenToClient(hwnd, &pt) && GetClientRect(hwnd, &rc) && rc.right > 0 && rc.bottom > 0 && pt.x >= 0 && pt.y >= 0 &&
                pt.x < rc.right && pt.y < rc.bottom)
            {
                s.ptrX = (float)pt.x / rc.right;
                s.ptrY = (float)pt.y / rc.bottom;
            }
        }
    }
    if (p.px >= 0) s.ptrX = p.px;
    if (p.py >= 0) s.ptrY = p.py;

    if (s.remote.t >= 0.0f) { s.remote.t += dt; if (s.remote.t > s.remote.dur) s.remote.t = -1.0f; }
    if (s.nunchuk.t >= 0.0f) { s.nunchuk.t += dt; if (s.nunchuk.t > s.nunchuk.dur) s.nunchuk.t = -1.0f; }
}

int Motion_GravityUnit(uint32_t) { return G; }

void Motion_FillStatus(int chan, uint32_t st, bool connected)
{
    State& s = s_state[chan & 3];
    const GamepadState& p = s.pad;
    memset(GuestPtr(st), 0, 0x36);
    if (!connected)
    {
        Write8(st + 0x28, 253);
        Write8(st + 0x29, (uint8_t)-1);
        return;
    }
    uint16_t b = 0;
    if (p.buttons & PAD_A) b |= WPB_A;
    if (p.buttons & PAD_B) b |= WPB_B;
    if (p.buttons & PAD_RB) b |= WPB_Z;
    if (p.buttons & PAD_LB) b |= WPB_C;
    if (p.buttons & PAD_START) b |= WPB_PLUS;
    if (p.buttons & PAD_BACK) b |= WPB_MINUS;
    if (p.buttons & PAD_HOME) b |= WPB_HOME;
    if (p.buttons & PAD_LTHUMB) b |= WPB_ONE;
    if (p.buttons & PAD_RTHUMB) b |= WPB_TWO;
    if (p.buttons & PAD_DPAD_UP) b |= WPB_UP;
    if (p.buttons & PAD_DPAD_DOWN) b |= WPB_DOWN;
    if (p.buttons & PAD_DPAD_LEFT) b |= WPB_LEFT;
    if (p.buttons & PAD_DPAD_RIGHT) b |= WPB_RIGHT;
    Write16(st + 0x00, b);

    // gravity as seen by the tilted Remote (as Dolphin computes it): forward tilt moves
    // it into +Y, a roll to the right into +X
    float up = std::cos(s.pitch);
    float gx = up * std::sin(s.roll), gy = std::sin(s.pitch), gz = up * std::cos(s.roll);
    float k = PulseShape(s.remote);
    int ax = (int)((gx + s.remote.ax * k) * G);
    int ay = (int)((gy + s.remote.ay * k) * G);
    int az = (int)((gz + s.remote.az * k) * G);
    Write16(st + 0x02, (uint16_t)(int16_t)std::clamp(ax, -511, 511));
    Write16(st + 0x04, (uint16_t)(int16_t)std::clamp(ay, -511, 511));
    Write16(st + 0x06, (uint16_t)(int16_t)std::clamp(az, -511, 511));

    // IR: two sensor bar dots. The mouse (inside the window) aims the pointer;
    // the camera sees the bar move opposite to where the remote points.
    float px = s.ptrX, py = s.ptrY;
    // Dot spacing sets KPAD's distance estimate (interval / spacing); the game
    // only accepts roughly 0.52..1.0, so 360 counts (~0.74) keeps it in range.
    // Calibrated against KPAD's output so the window maps onto pos -1..1
    // (the +102 cancels KPAD's sensor-bar-above offset).
    int cx = (int)(512 + (0.5f - px) * 460.0f);
    int cy = (int)(486 + (0.5f - py) * 452.0f);
    struct Dot { int x, y; } dots[2] = { { cx - 180, cy }, { cx + 180, cy } };
    for (int i = 0; i < 4; i++)
    {
        uint32_t o = st + 0x08 + i * 8;
        if (i < 2)
        {
            Write16(o + 0, (uint16_t)dots[i].x);
            Write16(o + 2, (uint16_t)dots[i].y);
            Write16(o + 4, 3);
            Write8(o + 6, (uint8_t)i);
        }
    }
    Write8(st + 0x28, 1);   // WPAD_DEV_FREESTYLE
    Write8(st + 0x29, 0);

    // Nunchuk
    float nk = PulseShape(s.nunchuk);
    Write16(st + 0x2A, (uint16_t)(int16_t)(int)(s.nunchuk.ax * nk * G));
    Write16(st + 0x2C, (uint16_t)(int16_t)0);
    Write16(st + 0x2E, (uint16_t)(int16_t)(G + (int)(s.nunchuk.az * nk * G)));
    Write8(st + 0x30, (uint8_t)(int8_t)std::clamp((int)std::lround(p.lx * 100.0f), -127, 127));
    Write8(st + 0x31, (uint8_t)(int8_t)std::clamp((int)std::lround(p.ly * 100.0f), -127, 127));
}
