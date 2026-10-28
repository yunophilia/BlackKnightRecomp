#include "log.h"
#include "platform/compat.h"
#include "platform/platform.h"

#include <cstdlib>
#include <mutex>

static std::mutex s_logMutex;
static FILE* s_logFile = nullptr;
int g_logLevel = 1;
bool g_noMessageBox = false;

void Log_Open(const char* path)
{
    // shared so the log can be read while the game is running
    s_logFile = _fsopen(path, "w", _SH_DENYWR);
}

void Log_Write(int level, const std::string& s)
{
    if (level < g_logLevel)
        return;
    static const char* kTag[] = { "D", "I", "W", "E" };
    std::lock_guard<std::mutex> lk(s_logMutex);
    fprintf(stdout, "[%s] %s\n", kTag[level & 3], s.c_str());
    if (s_logFile)
    {
        fprintf(s_logFile, "[%s] %s\n", kTag[level & 3], s.c_str());
        fflush(s_logFile);
    }
}

void Fatal(const std::string& s)
{
    Log_Write(3, "FATAL: " + s);
    fflush(stdout);
    if (!g_noMessageBox) Platform_ErrorBox("BlackKnightRecomp - fatal error", s);
    { void Gamepad_StopRumble(); Gamepad_StopRumble(); }
    Platform_ExitProcess(1);
}
