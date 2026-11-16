// BlackKnightRecomp: statically recompiled Sonic and the Black Knight (USA RENE8P,
// Europe RENP8P, Japan RENJ8P; the installed disc picks the recompiled code)
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
#include "mods/mods.h"
#include "region.h"
#include "platform/platform.h"

#include <algorithm>
#include <filesystem>
#include <string>

void Log_Open(const char* path);
void NAND_InitSystemFiles();
void Renderer_Init();
void AudioOut_Init();
void Crash_Init();
void Crash_StartWatchdog();
extern bool g_noMessageBox;
extern int g_logLevel;
extern "C" uint32_t g_cfgFrameRate;

// Restarts the game (settings that need a restart): same command line, no launcher.
void App_Restart()
{
    LOG_INFO("restarting");
    Platform_Restart(true, true);
}

// What the game thread needs from main()
static std::string s_dataDir, s_isoPath, s_nand;
static uint32_t s_watchAddr = 0;
static int s_profStart = -1, s_profDur = 0;

static void RunGame()
{
    if (!Memory_Init()) FATAL("memory init failed");
    Crash_Init();
    Cpu_Init();
    Sched_Init();
    Threads_Init();
    HW_Init();
    Renderer_Init();
    AudioOut_Init();
    { extern bool g_gpuThread; if (g_gpuThread) GX_StartThread(); }
    if (!Disc_Open(s_dataDir, s_isoPath, Mods_Loaded())) FATAL("cannot open disc data");
    std::filesystem::create_directories(s_nand);
    IOS_Init(s_nand);
    NAND_InitSystemFiles();

    uint32_t entry = Boot_Setup(s_dataDir);
    if (s_watchAddr) { extern void Crash_SetWatch(uint32_t); Crash_SetWatch(s_watchAddr); }
    if (!entry) FATAL("boot failed");
    Crash_StartWatchdog();
    if (s_profStart >= 0) { extern void Crash_StartProfiler(int, int); Crash_StartProfiler(s_profStart, s_profDur); }
    Threads_RunMain(entry);
}

int main(int argc, char** argv)
{
    Platform_Init(argc, argv);
    // packaging: fill the shader cache that ships with the exe (no window, no game data)
    if (argc == 3 && std::string(argv[1]) == "--precompile-shaders")
    {
        extern int Renderer_PrecompileShaders(const std::string& dir);
        return Renderer_PrecompileShaders(argv[2]) ? 1 : 0;
    }
    std::string root = Platform_UserDir().string();   // the exe directory on Windows
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
        else if (a == "--mods" && i + 1 < argc) Mods_SetDirOverride(argv[++i]);
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

    std::string gameId = Release_InstalledId(dataDir);
    {
        std::string error;
        if (!Region_Select(gameId, error)) FATAL("Cannot start the game: {}", error);
    }
    Mods_SetLoaded(Mods_Scan(Mods_Roots(g_config.modsDir), g_config.mods, gameId));
    { extern void SettingsUI_SetRelease(const std::string&, const std::string&); SettingsUI_SetRelease(gameId, dataDir); }
    s_dataDir = dataDir;
    s_isoPath = isoPath;
    s_nand = nandDir.empty() ? root + "/nand" : nandDir;
    s_watchAddr = watchAddr;
    s_profStart = profStart;
    s_profDur = profDur;
    Platform_RunGame(RunGame);
}
