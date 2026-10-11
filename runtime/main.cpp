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

int main(int argc, char** argv)
{
    std::string root = ExeDir();
    Log_Open((root + "/BlackKnightRecomp.log").c_str());
    Config_Load(root + "/config.ini");
    g_cfgFrameRate = g_config.frameRate;
    { extern bool g_gpuThread; g_gpuThread = g_config.gpuThread; }
    std::string dataDir = g_config.dataDir, isoPath = g_config.isoPath;
    uint32_t watchAddr = 0;
    int profStart = -1, profDur = 0;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--data" && i + 1 < argc) dataDir = argv[++i];
        else if (a == "--iso" && i + 1 < argc) isoPath = argv[++i];
        else if (a == "--verbose") g_logLevel = 0;
        else if (a == "--60fps") g_cfgFrameRate = 60;
        else if (a == "--profile" && i + 2 < argc) { profStart = atoi(argv[++i]); profDur = atoi(argv[++i]); }
        else if (a == "--dump-audio") { extern FILE* g_audioDump; g_audioDump = fopen("audio_dump.raw", "wb"); }
        else if (a == "--shots-start" && i + 1 < argc) { extern uint32_t g_screenshotStart; g_screenshotStart = (uint32_t)atoi(argv[++i]); }
        else if (a == "--dbg-nodepth") { extern int g_dbgNoDepth; g_dbgNoDepth = 1; }
        else if (a == "--dbg-nocull") { extern int g_dbgNoCull; g_dbgNoCull = 1; }
        else if (a == "--watch" && i + 1 < argc) watchAddr = (uint32_t)strtoul(argv[++i], nullptr, 16);
        else if (a == "--capture" && i + 1 < argc) { extern uint32_t g_captureFrame; g_captureFrame = (uint32_t)atoi(argv[++i]); }
        else if (a == "--nobox") g_noMessageBox = true;
        else if (a == "--single-core") { extern bool g_gpuThread; g_gpuThread = false; }
        else if (a == "--dump-textures") { extern bool g_dumpTextures; g_dumpTextures = true; }
        else if (a == "--autopress") { extern bool g_autoPress; g_autoPress = true; }
        else if (a == "--input-script" && i + 1 < argc) { extern void Gamepad_LoadScript(const char*); Gamepad_LoadScript(argv[++i]); }
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

    g_videoConfig.internalScale = g_config.internalScale;
    g_videoConfig.windowWidth = g_config.windowWidth;
    g_videoConfig.windowHeight = g_config.windowHeight;
    g_videoConfig.fullscreen = g_config.fullscreen;
    g_videoConfig.vsync = g_config.vsync;
    g_videoConfig.widescreen = g_config.widescreen;
    LOG_INFO("BlackKnightRecomp starting; data={} iso={} fps={} scale={}", dataDir, isoPath, g_cfgFrameRate, g_videoConfig.internalScale);
    if (dataDir.empty())
        FATAL("Game data not found. Extract the disc with DolphinTool into game/extracted (see README).");

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
    std::string nand = root + "/nand";
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
