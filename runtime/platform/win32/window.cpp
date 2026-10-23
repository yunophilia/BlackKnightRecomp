// The game window on Windows: its own thread runs the message loop.
#include "../window.h"
#include "../../config.h"
#include "../../log.h"
#include "../../ui/overlay.h"

#include <atomic>
#include <thread>
#include <windows.h>

bool Overlay_WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);   // platform/win32/ui.cpp

static HWND s_hwnd = nullptr;
static std::atomic<bool> s_resized{ false };
static bool s_fullscreen = false;   // window thread only
constexpr UINT WM_APP_FULLSCREEN = WM_APP + 1;   // wParam: 1 fullscreen, 0 windowed
constexpr UINT WM_APP_WINDOWSIZE = WM_APP + 2;   // wParam: width, lParam: height

// borderless fullscreen on the window's monitor, or back to the saved window rect
static void SetFullscreen(HWND hwnd, bool on)
{
    static RECT saved;
    if (on == s_fullscreen) return;
    s_fullscreen = on;
    if (on)
    {
        GetWindowRect(hwnd, &saved);
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    }
    else
    {
        SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(hwnd, nullptr, saved.left, saved.top, saved.right - saved.left, saved.bottom - saved.top, SWP_FRAMECHANGED);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (Overlay_WndProc(hwnd, msg, wp, lp)) return 0;   // the settings overlay has the input
    switch (msg)
    {
    case WM_CLOSE:
        // Closing the window quits. Saves are written to the host NAND as the game
        // issues them, so there is nothing to flush; terminate directly rather than
        // running DLL teardown while guest/GX/audio threads are mid-operation.
        LOG_INFO("window closed, exiting");
        { void Gamepad_StopRumble(); Gamepad_StopRumble(); }
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    case WM_SIZE:
        s_resized = true;
        return 0;
    case WM_ACTIVATEAPP:
        if (g_config.pauseOnFocusLoss)
        {
            extern std::atomic<bool> g_pauseRequested;
            g_pauseRequested = (wp == FALSE);
        }
        break;
    case WM_SETCURSOR:
        // the game draws its own Wii Remote pointer; the overlay needs the mouse cursor
        if (LOWORD(lp) == HTCLIENT)
        {
            SetCursor(Overlay_IsOpen() ? LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)) : nullptr);
            return TRUE;
        }
        break;
    case WM_APP_FULLSCREEN:
        SetFullscreen(hwnd, wp != 0);
        return 0;
    case WM_APP_WINDOWSIZE:
        if (!s_fullscreen)
        {
            RECT r{ 0, 0, (LONG)wp, (LONG)lp };
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_F11 || (wp == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000)))
        {
            SetFullscreen(hwnd, !s_fullscreen);
            g_config.fullscreen = s_fullscreen;
        }
        else if (wp == VK_F1)
            Overlay_Toggle();
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) { SendMessageW(hwnd, WM_KEYDOWN, VK_RETURN, lp); return 0; }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct WindowParams { int width, height; bool fullscreen; std::atomic<int> ready{ 0 }; };

static void WindowThread(WindowParams* p)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"BlackKnightRecomp";
    RegisterClassW(&wc);
    RECT r{ 0, 0, p->width, p->height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    s_hwnd = CreateWindowW(L"BlackKnightRecomp", L"Sonic and the Black Knight - Recompiled", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (s_hwnd && p->fullscreen) SetFullscreen(s_hwnd, true);
    p->ready = s_hwnd ? 1 : -1;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool Window_Create(int width, int height, bool fullscreen)
{
    static WindowParams p;
    p.width = width;
    p.height = height;
    p.fullscreen = fullscreen;
    std::thread(WindowThread, &p).detach();
    while (p.ready == 0) Sleep(1);
    if (p.ready < 0)
    {
        LOG_ERROR("CreateWindow failed ({})", GetLastError());
        return false;
    }
    return true;
}

void* Window_Handle() { return s_hwnd; }

void Window_ClientSize(int& width, int& height)
{
    RECT rc;
    GetClientRect(s_hwnd, &rc);
    width = rc.right - rc.left;
    height = rc.bottom - rc.top;
}

bool Window_TakeResized() { return s_resized.exchange(false); }

void Window_SetFullscreen(bool on) { if (s_hwnd) PostMessageW(s_hwnd, WM_APP_FULLSCREEN, on ? 1 : 0, 0); }
void Window_SetSize(int w, int h) { if (s_hwnd) PostMessageW(s_hwnd, WM_APP_WINDOWSIZE, (WPARAM)w, (LPARAM)h); }

bool Window_Focused() { return GetForegroundWindow() == s_hwnd; }

bool Window_Cursor(int& x, int& y)
{
    POINT pt;
    if (!GetCursorPos(&pt)) return false;
    x = pt.x;
    y = pt.y;
    return true;
}

bool Window_CursorInClient(int x, int y, float& u, float& v)
{
    POINT pt{ x, y };
    RECT rc;
    if (ScreenToClient(s_hwnd, &pt) && GetClientRect(s_hwnd, &rc) && rc.right > 0 && rc.bottom > 0 && pt.x >= 0 && pt.y >= 0 &&
        pt.x < rc.right && pt.y < rc.bottom)
    {
        u = (float)pt.x / rc.right;
        v = (float)pt.y / rc.bottom;
        return true;
    }
    return false;
}
