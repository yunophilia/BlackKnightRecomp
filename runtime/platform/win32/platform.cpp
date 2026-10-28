// Process, paths, time, threads and dialogs on Windows.
#include "../platform.h"
#include "../../log.h"

#include <algorithm>
#include <windows.h>
#include <timeapi.h>

namespace fs = std::filesystem;

void Gamepad_StopRumble();

// ---------------------------------------------------------------- process
void Platform_Init(int, char**)
{
    // Keep full timer resolution and CPU speed even when the window is minimized or in
    // the background (Windows 11 power throttling would coarsen all waits to ~15.6 ms).
    PROCESS_POWER_THROTTLING_STATE pt{};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    pt.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));
}

void Platform_RunGame(void (*game)())
{
    // the window has its own thread (gx/video.cpp); the game runs on the main thread,
    // whose stack is large (/STACK in CMakeLists.txt)
    game();
    Platform_ExitProcess(0);
}

// What a bug report needs to tell graphics problems apart: Wine/Proton (its D3D11 is DXVK
// or wined3d, its HLSL compiler vkd3d-shader) and which d3dcompiler DLL is loaded.
void Platform_LogHost()
{
    using WineVersionFn = const char*(__cdecl*)();
    if (auto wineVersion = (WineVersionFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version"))
        LOG_INFO("running under Wine {}", wineVersion());
    if (HMODULE dc = GetModuleHandleW(L"d3dcompiler_47.dll"))
    {
        wchar_t path[MAX_PATH];
        char utf8[MAX_PATH * 3];
        GetModuleFileNameW(dc, path, MAX_PATH);
        WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
        LOG_INFO("HLSL compiler: {}", utf8);
    }
}

void Platform_Terminate(int code)
{
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    for (;;) {}
}

void Platform_ExitProcess(int code) { ExitProcess((UINT)code); }

void Platform_Restart(bool skipLauncher, bool terminate)
{
    std::wstring cmd = GetCommandLineW();
    if (skipLauncher && cmd.find(L"--skip-launcher") == std::wstring::npos) cmd += L" --skip-launcher";
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    else
        LOG_WARN("restart failed ({})", GetLastError());
    Gamepad_StopRumble();
    if (terminate) Platform_Terminate(0);
    Platform_ExitProcess(0);
}

// ---------------------------------------------------------------- paths and files
fs::path Platform_ExeDir()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

fs::path Platform_UserDir() { return Platform_ExeDir(); }

fs::path Platform_DolphinTextureDir()
{
    const char* up = getenv("USERPROFILE");
    return up ? fs::path(up) / "Documents" / "Dolphin Emulator" / "Load" / "Textures" : fs::path();
}

bool Platform_ReplaceFile(const std::string& from, const std::string& to)
{
    return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::string Platform_UiFont()
{
    char dir[MAX_PATH];
    UINT n = GetWindowsDirectoryA(dir, MAX_PATH);
    std::string font = n ? std::string(dir, n) + "\\Fonts\\segoeui.ttf" : std::string();
    return !font.empty() && GetFileAttributesA(font.c_str()) != INVALID_FILE_ATTRIBUTES ? font : std::string();
}

std::string Platform_Utf8(const wchar_t* s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------- time
uint64_t Platform_TickMs() { return GetTickCount64(); }

uint64_t Platform_PerfCounter()
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return (uint64_t)q.QuadPart;
}

uint64_t Platform_PerfFrequency()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (uint64_t)f.QuadPart;
}

void Platform_SleepMs(uint32_t ms) { Sleep(ms); }

// ---------------------------------------------------------------- threads
void Thread_SetName(const char* name)
{
    wchar_t w[64];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 64);
    SetThreadDescription(GetCurrentThread(), w);
}

void Thread_SetPriority(ThreadPriority p)
{
    SetThreadPriority(GetCurrentThread(), p == ThreadPriority::TimeCritical ? THREAD_PRIORITY_TIME_CRITICAL : THREAD_PRIORITY_BELOW_NORMAL);
}

PlatformEvent::PlatformEvent() : impl(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
PlatformEvent::~PlatformEvent() { CloseHandle(impl); }
void PlatformEvent::Set() { SetEvent(impl); }
void PlatformEvent::Wait(uint32_t ms) { WaitForSingleObject(impl, ms); }

// Waits use a high-resolution waitable timer: plain Sleep/WaitForSingleObject follow
// the system tick, which Windows 11 keeps at ~15.6 ms for minimized or throttled
// processes regardless of timeBeginPeriod. Late events made the game's audio replay
// stale buffers (stutter) and could delay VI interrupts.
static HANDLE s_hrTimer = nullptr;

void Platform_TimerThreadSetup()
{
    Thread_SetPriority(ThreadPriority::TimeCritical);
    timeBeginPeriod(1);
    s_hrTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!s_hrTimer) LOG_WARN("scheduler: no high-resolution timer, timing may be coarse");
}

void Platform_TimerWait(PlatformEvent& wake, uint64_t hns)
{
    if (s_hrTimer)
    {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)hns;   // relative
        if (due.QuadPart > -1) due.QuadPart = -1;
        SetWaitableTimerEx(s_hrTimer, &due, 0, nullptr, nullptr, nullptr, 0);
        HANDLE hs[2] = { wake.impl, s_hrTimer };
        WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        CancelWaitableTimer(s_hrTimer);
    }
    else
        WaitForSingleObject(wake.impl, (DWORD)std::max<uint64_t>(1, hns / 10000));
}

// ---------------------------------------------------------------- memory
void* Platform_AllocPages(size_t bytes) { return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }

// ---------------------------------------------------------------- dialogs
static std::wstring Widen(const std::string& s)
{
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), (int)w.size());
    return w;
}

MsgResult Platform_MessageBox(const std::string& title, const std::string& text, MsgButtons buttons, MsgIcon icon)
{
    UINT type = buttons == MsgButtons::YesNoCancel ? MB_YESNOCANCEL : buttons == MsgButtons::OkCancel ? MB_OKCANCEL : MB_OK;
    type |= icon == MsgIcon::Error ? MB_ICONERROR : icon == MsgIcon::Question ? MB_ICONQUESTION : MB_ICONINFORMATION;
    switch (MessageBoxW(nullptr, Widen(text).c_str(), Widen(title).c_str(), type))
    {
    case IDOK: return MsgResult::Ok;
    case IDYES: return MsgResult::Yes;
    case IDNO: return MsgResult::No;
    default: return MsgResult::Cancel;
    }
}

void Platform_ErrorBox(const std::string& title, const std::string& text)
{
    MessageBoxA(nullptr, text.c_str(), title.c_str(), MB_ICONERROR);
}
