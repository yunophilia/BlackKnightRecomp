#include "log.h"

#include <cstdlib>
#include <mutex>
#include <windows.h>

static std::mutex s_logMutex;
static FILE* s_logFile = nullptr;
int g_logLevel = 1;
bool g_noMessageBox = false;

void Log_Open(const char* path)
{
    fopen_s(&s_logFile, path, "w");
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
    if (!g_noMessageBox) MessageBoxA(nullptr, s.c_str(), "BlackKnightRecomp - fatal error", MB_ICONERROR);
    ExitProcess(1);
}
