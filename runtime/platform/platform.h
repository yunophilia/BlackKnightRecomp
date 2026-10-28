#pragma once
// Operating system services used by the runtime. Each platform implements them once:
// platform/win32 (Win32 API) and platform/linux (POSIX and SDL3).
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

// ---------------------------------------------------------------- process
// Called first thing in main(): keeps the command line for restarts, and on Windows
// opts out of power throttling.
void Platform_Init(int argc, char** argv);
// Runs the game (guest code never returns). Windows: on this thread. Linux: on a thread
// with a large stack, while this (the main) thread handles the window's events.
[[noreturn]] void Platform_RunGame(void (*game)());
// Logs what a bug report needs to know about the host (Wine, HLSL compiler, ...).
void Platform_LogHost();
// Ends the process at once, without running any cleanup (other threads may be in the
// middle of something).
[[noreturn]] void Platform_Terminate(int code);
// Ends the process the normal way (Windows: ExitProcess).
[[noreturn]] void Platform_ExitProcess(int code);
// Starts the game again with the same command line (plus --skip-launcher when asked),
// then ends this process (terminate: as Platform_Terminate, else Platform_ExitProcess).
[[noreturn]] void Platform_Restart(bool skipLauncher, bool terminate);

// ---------------------------------------------------------------- paths and files
// The directory of the executable (read-only files shipped with it).
std::filesystem::path Platform_ExeDir();
// Where the game writes: log, config.ini, saves (nand), shader cache, screenshots and,
// by default, the installed game data. The exe directory on Windows; on Linux too for a
// plain build, but $XDG_DATA_HOME/BlackKnightRecomp when it runs from an AppImage or its
// directory is read-only.
std::filesystem::path Platform_UserDir();
// Dolphin's custom texture folder (Load/Textures) for this user.
std::filesystem::path Platform_DolphinTextureDir();
// Replaces `to` with `from` in one step (config.ini is written to a temporary file first).
bool Platform_ReplaceFile(const std::string& from, const std::string& to);
// A UI font of the system (empty: use ImGui's own).
std::string Platform_UiFont();
// UTF-8 of a wide string (UTF-16 on Windows, UTF-32 elsewhere).
std::string Platform_Utf8(const wchar_t* s);

// ---------------------------------------------------------------- time
uint64_t Platform_TickMs();          // milliseconds since some point (GetTickCount64)
uint64_t Platform_PerfCounter();     // high-resolution counter
uint64_t Platform_PerfFrequency();   // its ticks per second
void Platform_SleepMs(uint32_t ms);

// ---------------------------------------------------------------- threads
enum class ThreadPriority { BelowNormal, TimeCritical };
void Thread_SetName(const char* name);
void Thread_SetPriority(ThreadPriority p);

// Auto-reset event: Set wakes one waiter (or the next one to wait).
class PlatformEvent
{
public:
    PlatformEvent();
    ~PlatformEvent();
    PlatformEvent(const PlatformEvent&) = delete;
    PlatformEvent& operator=(const PlatformEvent&) = delete;
    void Set();
    void Wait(uint32_t ms);
    void* impl;
};

// The scheduler's timer thread: high priority and waits that are precise to well under a
// millisecond even when the window is minimized.
void Platform_TimerThreadSetup();
// Waits until `wake` is set or `hns` (100 ns units) have passed.
void Platform_TimerWait(PlatformEvent& wake, uint64_t hns);

// ---------------------------------------------------------------- memory
// Zeroed, page-aligned memory that is never freed.
void* Platform_AllocPages(size_t bytes);

// ---------------------------------------------------------------- dialogs
enum class MsgButtons { Ok, OkCancel, YesNoCancel };
enum class MsgIcon { Info, Question, Error };
enum class MsgResult { Ok, Cancel, Yes, No };
MsgResult Platform_MessageBox(const std::string& title, const std::string& text, MsgButtons buttons, MsgIcon icon);
// The fatal error / crash box (no buttons to choose from).
void Platform_ErrorBox(const std::string& title, const std::string& text);
