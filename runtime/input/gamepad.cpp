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

// Players 2-4 exist only while their XInput pad is plugged in. Querying an empty slot
// is slow, so absent pads are re-checked at most once a second.
bool Gamepad_IsConnected(int index)
{
    if (index == 0) return true;   // keyboard fallback
    static DWORD lastCheck[4];
    static bool connected[4];
    DWORD now = GetTickCount();
    if (!connected[index] && lastCheck[index] && now - lastCheck[index] < 1000) return false;
    XINPUT_STATE st{};
    connected[index] = XInputGetState(index, &st) == ERROR_SUCCESS;
    lastCheck[index] = now;
    return connected[index];
}

bool g_autoPress = false;   // test aid: tap A every few seconds

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
        out.buttons |= st.Gamepad.wButtons & ~PAD_HOME;
        // Back+Start together act as the Wii Remote HOME button
        if ((out.buttons & (PAD_BACK | PAD_START)) == (PAD_BACK | PAD_START))
            out.buttons = (out.buttons & ~(PAD_BACK | PAD_START)) | PAD_HOME;
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
