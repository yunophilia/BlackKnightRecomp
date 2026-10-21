// BlackKnightRecomp: statically recompiled Sonic and the Black Knight (RENE8P)
#include "log.h"
#include "config.h"
#include "gx/video.h"
#include "cpu/cpu.h"
#include "kernel/memory.h"
#include "kernel/boot.h"
#include "kernel/scheduler.h"
#include "kernel/threads.h"
#include "hw/hw.h"
#include "ios/disc.h"
#include "ios/ios.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <windows.h>

void Log_Open(const char* path);
void NAND_InitSystemFiles();
void Renderer_Init();
void AudioOut_Init();
void Crash_Init();
void Crash_StartWatchdog();
extern bool g_noMessageBox;
extern int g_logLevel;
extern "C" uint32_t g_cfgFrameRate;

static std::string ExeDir()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path().string();
}

// Restarts the game (settings that need a restart): same command line, no launcher.
void App_Restart()
{
    std::wstring cmd = GetCommandLineW();
    if (cmd.find(L"--skip-launcher") == std::wstring::npos) cmd += L" --skip-launcher";
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    LOG_INFO("restarting");
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    else
        LOG_WARN("restart failed ({})", GetLastError());
    { void Gamepad_StopRumble(); Gamepad_StopRumble(); }
    TerminateProcess(GetCurrentProcess(), 0);
}

int main(int argc, char** argv)
{
    // Keep full timer resolution and CPU speed even when the window is minimized or in
    // the background (Windows 11 power throttling would coarsen all waits to ~15.6 ms).
    {
        PROCESS_POWER_THROTTLING_STATE pt{};
        pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        pt.StateMask = 0;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));
    }
    // packaging: fill the shader cache that ships with the exe (no window, no game data)
    if (argc == 3 && std::string(argv[1]) == "--precompile-shaders")
    {
        extern int Renderer_PrecompileShaders(const std::string& dir);
        return Renderer_PrecompileShaders(argv[2]) ? 1 : 0;
    }
    std::string root = ExeDir();
    Log_Open((root + "/BlackKnightRecomp.log").c_str());
    Config_Load(root + "/config.ini");
    std::string dataDir = g_config.dataDir, isoPath = g_config.isoPath;
    uint32_t watchAddr = 0;
    int profStart = -1, profDur = 0;
    std::string nandDir;   // --nand: separate save data (test runs)
    // command line overrides for this run only (never written to config.ini)
    bool fps60 = false, singleCore = false, skipLauncher = false;
    int scaleOverride = 0;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--data" && i + 1 < argc) dataDir = argv[++i];
        else if (a == "--iso" && i + 1 < argc) isoPath = argv[++i];
        else if (a == "--verbose") g_logLevel = 0;
        else if (a == "--60fps") fps60 = true;
        else if (a == "--profile" && i + 2 < argc) { profStart = atoi(argv[++i]); profDur = atoi(argv[++i]); }
        else if (a == "--dump-audio") { extern FILE* g_audioDump; g_audioDump = fopen("audio_dump.raw", "wb"); }
        else if (a == "--shots-start" && i + 1 < argc) { extern uint32_t g_screenshotStart; g_screenshotStart = (uint32_t)atoi(argv[++i]); }
        else if (a == "--dbg-nodepth") { extern int g_dbgNoDepth; g_dbgNoDepth = 1; }
        else if (a == "--dbg-nocull") { extern int g_dbgNoCull; g_dbgNoCull = 1; }
        else if (a == "--watch" && i + 1 < argc) watchAddr = (uint32_t)strtoul(argv[++i], nullptr, 16);
        else if (a == "--capture" && i + 1 < argc) { extern uint32_t g_captureFrame; g_captureFrame = (uint32_t)atoi(argv[++i]); }
        else if (a == "--nobox") g_noMessageBox = true;
        else if (a == "--profile-gx") { extern bool g_profileGX; g_profileGX = true; }
        else if (a == "--single-core") singleCore = true;
        else if (a == "--skip-launcher") skipLauncher = true;
        else if (a == "--dump-textures") { extern bool g_dumpTextures; g_dumpTextures = true; }
        else if (a == "--autopress") { extern bool g_autoPress; g_autoPress = true; }
        else if (a == "--input-script" && i + 1 < argc) { extern void Gamepad_LoadScript(const char*); Gamepad_LoadScript(argv[++i]); }
        else if (a == "--tas" && i + 1 < argc) { extern void Tas_Load(const char*); Tas_Load(argv[++i]); }
        else if (a == "--telemetry" && i + 1 < argc) { extern void Tas_SetTelemetry(const char*); Tas_SetTelemetry(argv[++i]); }
        else if (a == "--nand" && i + 1 < argc) nandDir = argv[++i];
        else if (a == "--mute") { extern bool g_audioMute; g_audioMute = true; }
        else if (a == "--scale" && i + 1 < argc) scaleOverride = std::clamp(atoi(argv[++i]), 1, 8);
        else if (a == "--shots" && i + 1 < argc) { extern int g_screenshotEvery; g_screenshotEvery = atoi(argv[++i]); }
    }
    // Default locations: <exe>/game/... or the repo layout
    auto pick = [&](std::string& v, const char* rel) {
        if (!v.empty()) return;
        for (std::string base : { root, root + "/..", root + "/../..", root + "/../../.." })
        {
            std::filesystem::path p = std::filesystem::path(base) / rel;
            if (std::filesystem::exists(p)) { v = std::filesystem::weakly_canonical(p).string(); return; }
        }
    };
    pick(dataDir, "game/extracted/DATA");
    pick(isoPath, "game/RENE8P_Sonic_and_the_Black_Knight.iso");
    if (isoPath.empty())
    {
        // any disc image in game/ next to the exe or the repo root
        for (std::string b : { root, root + "/.." })
        {
            std::error_code ec;
            for (auto& e : std::filesystem::directory_iterator(std::filesystem::path(b) / "game", ec))
            {
                auto ext = e.path().extension().string();
                if (ext == ".iso" || ext == ".ISO") { isoPath = e.path().string(); break; }
            }
            if (!isoPath.empty()) break;
        }
    }

    // installs go to game/ next to the exe (or the repo's game/ when running from out/)
    std::filesystem::path gameDir = std::filesystem::path(root) / "game";
    if (!std::filesystem::exists(gameDir) && std::filesystem::exists(std::filesystem::path(root) / ".." / "game"))
        gameDir = std::filesystem::weakly_canonical(std::filesystem::path(root) / ".." / "game");
    std::filesystem::path installDest = gameDir / "extracted" / "DATA";

    // Launcher on every start (settings, install / repair the game data); TAS runs,
    // --skip-launcher (test scripts) and restarts from the in-game settings go straight to
    // the game. --nobox only suppresses message boxes and dialogs.
    extern bool Tas_Active();
    if (!Tas_Active() && !skipLauncher)
    {
        extern bool Launcher_Run(std::string& dataDir, const std::string& isoPath, const std::filesystem::path& installDest);
        if (!Launcher_Run(dataDir, isoPath, installDest)) return 0;
    }

    g_cfgFrameRate = fps60 ? 60 : g_config.frameRate;
    { extern bool g_gpuThread; g_gpuThread = g_config.gpuThread && !singleCore; }
    g_videoConfig.internalScale = scaleOverride ? scaleOverride : g_config.internalScale;
    g_videoConfig.windowWidth = g_config.windowWidth;
    g_videoConfig.windowHeight = g_config.windowHeight;
    g_videoConfig.fullscreen = g_config.fullscreen;
    g_videoConfig.vsync = g_config.vsync;
    g_videoConfig.widescreen = g_config.widescreen;
    LOG_INFO("BlackKnightRecomp starting; data={} iso={} fps={} scale={}", dataDir, isoPath, g_cfgFrameRate, g_videoConfig.internalScale);
    if (dataDir.empty())
    {
        // no launcher (restart, --skip-launcher): install directly
        if (g_noMessageBox)
            FATAL("Game data not installed. Start the game once normally to install it from your disc image, "
                  "or run bkr_install <disc image> (see README).");
        extern bool InstallUI_Run(const std::filesystem::path&, const std::string&, const std::string&);
        if (!InstallUI_Run(installDest, isoPath, "")) return 1;
        dataDir = installDest.string();
    }

    if (!Memory_Init()) FATAL("memory init failed");
    Crash_Init();
    Cpu_Init();
    Sched_Init();
    Threads_Init();
    HW_Init();
    Renderer_Init();
    AudioOut_Init();
    { extern bool g_gpuThread; if (g_gpuThread) GX_StartThread(); }
    if (!Disc_Open(dataDir, isoPath)) FATAL("cannot open disc data");
    std::string nand = nandDir.empty() ? root + "/nand" : nandDir;
    std::filesystem::create_directories(nand);
    IOS_Init(nand);
    NAND_InitSystemFiles();

    uint32_t entry = Boot_Setup(dataDir);
    if (watchAddr) { extern void Crash_SetWatch(uint32_t); Crash_SetWatch(watchAddr); }
    if (!entry) FATAL("boot failed");
    Crash_StartWatchdog();
    if (profStart >= 0) { extern void Crash_StartProfiler(int, int); Crash_StartProfiler(profStart, profDur); }
    Threads_RunMain(entry);
    return 0;
}
