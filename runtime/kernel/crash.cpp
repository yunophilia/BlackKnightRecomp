// Crash reporting: maps host faults back to guest functions and dumps guest state. The
// fault handler, host stack walks and the sampling profilers are in platform/*/crash.cpp.
#include "crash.h"
#include "threads.h"
#include "memory.h"
#include "../log.h"
#include "../cpu/cpu.h"
#include "../platform/platform.h"
#include "../region.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

static std::vector<std::pair<uintptr_t, uint32_t>> s_hostToGuest;

void Crash_BuildMap()
{
    for (const PPCFuncMapping* m = Region_Code().funcs; m->host; m++)
        s_hostToGuest.push_back({ (uintptr_t)m->host, m->guest });
    std::sort(s_hostToGuest.begin(), s_hostToGuest.end());
}

uint32_t Crash_GuestFunctionForHost(uintptr_t rip)
{
    auto it = std::upper_bound(s_hostToGuest.begin(), s_hostToGuest.end(), std::make_pair(rip, 0xFFFFFFFFu));
    if (it == s_hostToGuest.begin()) return 0;
    --it;
    return it->second;
}

uintptr_t Crash_FirstHostFunction() { return s_hostToGuest.empty() ? 0 : s_hostToGuest[0].first; }

void Crash_DumpContext(const PPCContext& c)
{
    for (int i = 0; i < 32; i += 4)
        LOG_ERROR("  r{:<2}={:08x} r{:<2}={:08x} r{:<2}={:08x} r{:<2}={:08x}", i, c.r[i], i + 1, c.r[i + 1], i + 2, c.r[i + 2], i + 3, c.r[i + 3]);
    LOG_ERROR("  lr={:08x} ctr={:08x} msr={:08x}", c.lr, c.ctr, c.msr);
}

// ---------------------------------------------------------------- watchdog
extern uint32_t g_framesPresented;

static void WatchdogThread()
{
    uint32_t lastFrames = 0;
    for (;;)
    {
        Platform_SleepMs(3000);
        if (!Crash_GuestThreadKnown()) continue;
        // sample a few times and report the most frequent guest functions
        uint32_t samples[16];
        for (int i = 0; i < 16; i++)
        {
            samples[i] = Crash_GuestFunctionForHost(Crash_SampleGuestPC());
            Platform_SleepMs(5);
        }
        std::string s;
        for (int i = 0; i < 16; i++) s += std::format("{:08X} ", samples[i]);
        extern uint32_t g_excCount[16];
        extern uint32_t PI_Read32(uint32_t off);
        extern uint32_t g_viFieldCount;
        {
            extern int g_audioPeak;
            static uint32_t lastFields = 0;
            static uint64_t lastQpc = 0;
            uint64_t q = Platform_PerfCounter();
            double secs = lastQpc ? (double)(q - lastQpc) / Platform_PerfFrequency() : 3.0;
            lastQpc = q;
            extern int g_audioUnderruns, g_audioDrops, g_audioRepeats;
            extern uint64_t g_audioFrames;
            static uint64_t lastAudio = 0;
            LOG_INFO("status: {:.1f} fps, VI {:.2f} Hz, audio peak {} underruns {} drops {} repeats {} rate {:.0f} Hz", (g_framesPresented - lastFrames) / secs,
                     (g_viFieldCount - lastFields) / secs, g_audioPeak, g_audioUnderruns, g_audioDrops, g_audioRepeats, (g_audioFrames - lastAudio) / secs);
            g_audioRepeats = 0;
            lastAudio = g_audioFrames;
            g_audioUnderruns = g_audioDrops = 0;
            lastFields = g_viFieldCount;
            g_audioPeak = 0;
        }
        if (g_framesPresented == lastFrames)
        {
            LOG_INFO("watchdog: no new frames; guest at: {}", s);
            LOG_INFO("  exceptions: ext={} dec={}  PI cause={:08x} mask={:08x}  VI fields={}  curThread={:08x}", g_excCount[4], g_excCount[8],
                     PI_Read32(0), PI_Read32(4), g_viFieldCount, Read32(0x800000E4));
            extern uint16_t VI_Read16(uint32_t off);
            LOG_INFO("  VI: DI0={:04x}{:04x} DI1={:04x}{:04x} DI2={:04x}{:04x} DI3={:04x}{:04x} vtr={:04x} dcr={:04x} beam={}", VI_Read16(0x30), VI_Read16(0x32),
                     VI_Read16(0x34), VI_Read16(0x36), VI_Read16(0x38), VI_Read16(0x3A), VI_Read16(0x3C), VI_Read16(0x3E), VI_Read16(0), VI_Read16(2), VI_Read16(0x2C));
            bool armed; extern uint64_t Dec_Deadline(bool*); uint64_t dl = Dec_Deadline(&armed);
            LOG_INFO("  DEC armed={} remaining={}", armed, (int64_t)(dl - Timebase_Now()));
            if (g_curThread)
            {
                const PPCContext& c = g_curThread->ctx;
                std::string bt;
                uint32_t fp = c.r[1];
                for (int i = 0; i < 12 && fp >= 0x80000000 && fp < 0x93600000; i++) { uint32_t nx = Read32(fp); if (nx < 0x80000000 || nx >= 0x93600000) break; bt += std::format("{:08x} ", Read32(nx + 4)); fp = nx; }
                LOG_INFO("  current fiber: lr={:08x} msr={:08x} r1={:08x} r3={:08x} chain: {}", c.lr, c.msr, c.r[1], c.r[3], bt);
            }
            extern void Crash_DumpThreads();
            Crash_DumpThreads();
        }
        lastFrames = g_framesPresented;
    }
}

// BKR_IRQ_STALLS=1: sample where the guest thread is when it has not polled interrupts

void Crash_StartWatchdog()
{
    Crash_StartSamplers();
    std::thread(WatchdogThread).detach();
    // BKR_TEST_PAUSE=start,end (seconds): exercises the focus-loss pause without
    // touching window focus
    if (const char* tp = getenv("BKR_TEST_PAUSE"))
    {
        int a = 0, b = 0;
        if (sscanf(tp, "%d,%d", &a, &b) == 2 && b > a)
            std::thread([a, b] {
                extern std::atomic<bool> g_pauseRequested;
                Platform_SleepMs(a * 1000);
                g_pauseRequested = true;
                Platform_SleepMs((b - a) * 1000);
                g_pauseRequested = false;
            }).detach();
    }
}

// Walk the OS active thread queue and print each thread with a back-chain trace.
void Crash_DumpThreads()
{
    uint32_t t = Read32(0x800000DC);
    for (int n = 0; t && n < 32; n++)
    {
        uint16_t state = Read16(t + 0x2C8);
        int32_t prio = (int32_t)Read32(t + 0x2D0);
        uint32_t queue = Read32(t + 0x2DC);
        uint32_t sp = Read32(t + 4);
        std::string bt;
        uint32_t fp = sp;
        for (int i = 0; i < 10 && fp >= 0x80000000 && fp < 0x81800000; i++)
        {
            uint32_t next = Read32(fp);
            if (next < 0x80000000 || next >= 0x81800000) break;
            bt += std::format("{:08x} ", Read32(next + 4));
            fp = next;
        }
        LOG_INFO("  thread {:08x} state={} prio={} queue={:08x} sp={:08x} lr-chain: {}", t, state, prio, queue, sp, bt);
        t = Read32(t + 0x2FC);
    }
}
