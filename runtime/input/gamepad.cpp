// XInput controllers with a keyboard fallback for player 1.
#include "gamepad.h"
#include "../config.h"
#include "../tas/tas.h"

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

// Players 2-4 exist only while their XInput pad is plugged in. Querying an empty slot
// is slow, so absent pads are re-checked at most once a second.
bool Gamepad_IsConnected(int index)
{
    if (index == 0) return true;   // keyboard fallback
    if (Tas_Active()) return false;   // scripted runs ignore real controllers
    static DWORD lastCheck[4];
    static bool connected[4];
    DWORD now = GetTickCount();
    if (!connected[index] && lastCheck[index] && now - lastCheck[index] < 1000) return false;
    XINPUT_STATE st{};
    connected[index] = XInputGetState(index, &st) == ERROR_SUCCESS;
    lastCheck[index] = now;
    return connected[index];
}

// Button bindings from config.ini: physical controller buttons -> the logical layout
// the rest of the input code uses (A jump, X slash, B Wii B, ...).
static uint32_t PhysicalBit(const std::string& name)
{
    static const struct { const char* n; uint32_t b; } names[] = {
        { "A", PAD_A }, { "B", PAD_B }, { "X", PAD_X }, { "Y", PAD_Y }, { "LB", PAD_LB }, { "RB", PAD_RB },
        { "START", PAD_START }, { "BACK", PAD_BACK }, { "LS", PAD_LTHUMB }, { "RS", PAD_RTHUMB },
        { "UP", PAD_DPAD_UP }, { "DOWN", PAD_DPAD_DOWN }, { "LEFT", PAD_DPAD_LEFT }, { "RIGHT", PAD_DPAD_RIGHT },
        { "LT", 1u << 16 }, { "RT", 1u << 17 },
    };
    std::string u;
    for (char c : name) u += (char)toupper((unsigned char)c);
    for (auto& n : names)
        if (u == n.n) return n.b;
    return 0;
}

static void ApplyBindings(GamepadState& out, uint32_t phys, float lt, float rt)
{
    static uint32_t mask[12];
    static bool init = false;
    if (!init)
    {
        for (int i = 0; i < 12; i++) mask[i] = PhysicalBit(g_config.bind[i]);
        init = true;
    }
    static const uint16_t logical[10] = { PAD_A, PAD_X, PAD_B, PAD_Y, PAD_RB, PAD_LB, PAD_START, PAD_BACK, PAD_LTHUMB, PAD_RTHUMB };
    uint16_t b = (uint16_t)(phys & (PAD_DPAD_UP | PAD_DPAD_DOWN | PAD_DPAD_LEFT | PAD_DPAD_RIGHT));
    for (int i = 0; i < 10; i++)
        if (phys & mask[i]) b |= logical[i];
    out.buttons |= b;
    // analog triggers keep their value when bound to themselves
    out.rt = mask[10] == (1u << 17) ? rt : ((phys & mask[10]) ? 1.0f : 0.0f);
    out.lt = mask[11] == (1u << 16) ? lt : ((phys & mask[11]) ? 1.0f : 0.0f);
}

bool g_autoPress = false;   // test aid: tap A every few seconds

// Rumble goes to the real controller, so it is only allowed when a person is playing:
// not during scripted/automated input, and only while the game window has focus.
bool Gamepad_RumbleAllowed();

// Test aid: timed input script. Each line: <start seconds> <duration> <BUTTONS|LX=v|LY=v ...>
// e.g. "12.0 0.2 A+B" or "30 2.0 LY=1.0"
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
struct ScriptEvent { double t0, t1; uint16_t buttons; float lx, ly, rx, ry, rt; bool hasStick; float px = -1, py = -1; };
static std::vector<ScriptEvent> s_script;
static DWORD s_scriptStart = 0;

void Gamepad_LoadScript(const char* path)
{
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        ScriptEvent e{};
        double dur;
        std::string what;
        ss >> e.t0 >> dur;
        e.t1 = e.t0 + dur;
        while (ss >> what)
        {
            std::istringstream parts(what);
            std::string tok;
            while (std::getline(parts, tok, '+'))
            {
                auto eq = tok.find('=');
                if (eq != std::string::npos)
                {
                    float v = std::stof(tok.substr(eq + 1));
                    std::string k = tok.substr(0, eq);
                    if (k == "PX") { e.px = v; continue; }
                    if (k == "PY") { e.py = v; continue; }
                    e.hasStick = true;
                    if (k == "LX") e.lx = v; else if (k == "LY") e.ly = v; else if (k == "RX") e.rx = v; else if (k == "RY") e.ry = v; else if (k == "RT") e.rt = v;
                    continue;
                }
                static const struct { const char* n; uint16_t b; } names[] = {
                    { "A", PAD_A }, { "B", PAD_B }, { "X", PAD_X }, { "Y", PAD_Y }, { "LB", PAD_LB }, { "RB", PAD_RB },
                    { "START", PAD_START }, { "BACK", PAD_BACK }, { "UP", PAD_DPAD_UP }, { "DOWN", PAD_DPAD_DOWN },
                    { "LEFT", PAD_DPAD_LEFT }, { "RIGHT", PAD_DPAD_RIGHT }, { "HOME", PAD_HOME },
                };
                for (auto& n : names) if (tok == n.n) e.buttons |= n.b;
            }
        }
        s_script.push_back(e);
    }
    s_scriptStart = GetTickCount();
}

void Gamepad_Poll(int index, GamepadState& out)
{
    out = GamepadState();
    if (index == 0 && Tas_Poll(out))
        return;
    if (Tas_Active())
        return;
    if (!s_script.empty() && index == 0)
    {
        double t = (GetTickCount() - s_scriptStart) / 1000.0;
        for (auto& e : s_script)
            if (t >= e.t0 && t < e.t1)
            {
                out.buttons |= e.buttons;
                if (e.hasStick) { out.lx = e.lx; out.ly = e.ly; out.rx = e.rx; out.ry = e.ry; out.rt = e.rt; }
                if (e.px >= 0) out.px = e.px;
                if (e.py >= 0) out.py = e.py;
            }
        out.connected = true;
        // Script is authoritative; skip real devices so a script run is reproducible
        return;
    }
    if (g_autoPress && index == 0)
    {
        DWORD t = GetTickCount() % 3000;
        if (t < 150) out.buttons |= PAD_A;
        out.connected = true;
    }
    XINPUT_STATE st{};
    if ((index == 0 || Gamepad_IsConnected(index)) && XInputGetState(index, &st) == ERROR_SUCCESS)
    {
        out.connected = true;
        float lt = st.Gamepad.bLeftTrigger / 255.0f, rt = st.Gamepad.bRightTrigger / 255.0f;
        uint32_t phys = st.Gamepad.wButtons & ~PAD_HOME;
        if (lt > 0.6f) phys |= 1u << 16;
        if (rt > 0.6f) phys |= 1u << 17;
        // Back+Start together act as the Wii Remote HOME button
        if ((phys & (PAD_BACK | PAD_START)) == (PAD_BACK | PAD_START))
        {
            phys &= ~(PAD_BACK | PAD_START);
            out.buttons |= PAD_HOME;
        }
        ApplyBindings(out, phys, lt, rt);
        out.lx = Stick(st.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        out.ly = Stick(st.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        out.rx = Stick(st.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        out.ry = Stick(st.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
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
    map(VK_LBUTTON, PAD_X);    // mouse: left click slashes, right click guards
    map(VK_RBUTTON, PAD_RB);
    map(VK_RETURN, PAD_START);
    map(VK_BACK, PAD_BACK);
    map('H', PAD_HOME);
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

bool Gamepad_RumbleAllowed()
{
    if (!s_script.empty() || g_autoPress || Tas_Active()) return false;
    HWND hwnd = (HWND)Video_HWND();
    return hwnd && GetForegroundWindow() == hwnd;
}

// Stops all controller motors (called on focus loss and before exiting).
void Gamepad_StopRumble()
{
    XINPUT_VIBRATION v{};
    for (DWORD i = 0; i < 4; i++) XInputSetState(i, &v);
}
