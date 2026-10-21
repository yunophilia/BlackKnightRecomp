// Crash reporting: maps host faults back to guest functions and dumps guest state.
#include "threads.h"
#include "memory.h"
#include "../log.h"
#include "../cpu/cpu.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#pragma comment(lib, "dbghelp.lib")

extern "C" const PPCFuncMapping PPCFuncMappings[];

static std::vector<std::pair<uintptr_t, uint32_t>> s_hostToGuest;

static void BuildMap()
{
    for (const PPCFuncMapping* m = PPCFuncMappings; m->host; m++)
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

static void DumpContext(const PPCContext& c)
{
    for (int i = 0; i < 32; i += 4)
        LOG_ERROR("  r{:<2}={:08x} r{:<2}={:08x} r{:<2}={:08x} r{:<2}={:08x}", i, c.r[i], i + 1, c.r[i + 1], i + 2, c.r[i + 2], i + 3, c.r[i + 3]);
    LOG_ERROR("  lr={:08x} ctr={:08x} msr={:08x}", c.lr, c.ctr, c.msr);
}

static LONG CrashReport(EXCEPTION_POINTERS* ep);
struct CrashJob { EXCEPTION_POINTERS* ep; void* frames[62]; USHORT n; };
static CrashJob s_job;
static DWORD WINAPI CrashThread(void*) { CrashReport(s_job.ep); return 0; }

// ---------------------------------------------------------------- debug write watch
static uint32_t s_watchAddr = 0;
static int s_watchHits = 0;
static void ProtectWatch(DWORD prot)
{
    uint32_t phys = s_watchAddr & 0x1FFFFFFF;
    uint32_t views[3] = { 0x80000000 | phys, 0xC0000000 | phys, phys };
    for (uint32_t v : views)
    {
        DWORD old;
        VirtualProtect(g_base + (v & ~0xFFFu), 0x1000, prot, &old);
    }
}
void Crash_SetWatch(uint32_t addr)
{
    s_watchAddr = addr;
    ProtectWatch(PAGE_READONLY);
    LOG_INFO("watching writes to guest page of {:08x}", addr);
}

static LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* ep)
{
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c == EXCEPTION_SINGLE_STEP && s_watchAddr)
    {
        ProtectWatch(PAGE_READONLY);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (c == EXCEPTION_ACCESS_VIOLATION && s_watchAddr && ep->ExceptionRecord->ExceptionInformation[0] == 1)
    {
        uintptr_t fa = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        uint32_t ga = (uint32_t)(fa - (uintptr_t)g_base);
        if ((ga & 0x1FFFF000) == (s_watchAddr & 0x1FFFF000))
        {
            uint32_t fn = Crash_GuestFunctionForHost((uintptr_t)ep->ContextRecord->Rip);
            static uint32_t lastFn = 0, lastLr = 0;
            uint32_t lr = g_curThread ? g_curThread->ctx.lr : 0;
            bool exact = ga - (s_watchAddr & 0x1FFFFFFFu) < 4 || ga - s_watchAddr < 4;   // only the watched word is logged
            if (exact && (fn != lastFn || lr != lastLr) && s_watchHits++ < 40)
                LOG_INFO("WATCH: write to {:08x} by ~fn_{:08X} lr={:08x} r3={:08x} r4={:08x} r5={:08x}", ga, fn, lr,
                         g_curThread ? g_curThread->ctx.r[3] : 0, g_curThread ? g_curThread->ctx.r[4] : 0, g_curThread ? g_curThread->ctx.r[5] : 0);
            lastFn = fn;
            lastLr = lr;
            ProtectWatch(PAGE_READWRITE);
            ep->ContextRecord->EFlags |= 0x100;   // single-step, then re-arm
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (c != EXCEPTION_ACCESS_VIOLATION && c != EXCEPTION_ILLEGAL_INSTRUCTION && c != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        c != EXCEPTION_STACK_OVERFLOW && c != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    // Capture the faulting stack here (cheap), report from a fresh thread (safe even on stack overflow)
    s_job.ep = ep;
    s_job.n = RtlCaptureStackBackTrace(0, 62, s_job.frames, nullptr);
    HANDLE h = CreateThread(nullptr, 4 << 20, CrashThread, nullptr, 0, nullptr);
    WaitForSingleObject(h, 60000);
    { void Gamepad_StopRumble(); Gamepad_StopRumble(); }
    TerminateProcess(GetCurrentProcess(), 2);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG CrashReport(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    static bool inHandler = false;
    if (inHandler) return EXCEPTION_CONTINUE_SEARCH;
    inHandler = true;
    uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
    uintptr_t addr = code == EXCEPTION_ACCESS_VIOLATION ? (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1] : 0;
    LOG_ERROR("=== host exception {:08x} at rip={:x} (fault addr {:x}) ===", (uint32_t)code, rip, addr);
    if (g_base && addr >= (uintptr_t)g_base && addr < (uintptr_t)g_base + 0x100000000ull)
        LOG_ERROR("  guest address {:08x}", (uint32_t)(addr - (uintptr_t)g_base));
    uint32_t gf = Crash_GuestFunctionForHost(rip);
    LOG_ERROR("  in guest function ~fn_{:08X} (host offset +{:x})", gf, gf ? rip - (uintptr_t)s_hostToGuest[0].first : 0);
    // Walk the host stack for more guest frames
    void** frames = s_job.frames;
    USHORT n = s_job.n;
    static bool symInit = false;
    if (!symInit) { SymInitialize(GetCurrentProcess(), nullptr, TRUE); symInit = true; }
    for (USHORT i = 0; i < n; i++)
    {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        const char* nm = SymFromAddr(GetCurrentProcess(), (DWORD64)frames[i], &disp, si) ? si->Name : "?";
        // the module, for frames in system or driver DLLs without symbols (Wine, DXVK)
        char mod[MAX_PATH] = "?";
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &hm))
            GetModuleFileNameA(hm, mod, sizeof(mod));
        const char* base = strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod;
        LOG_ERROR("  host frame {:2}: {} {}+{:x} ({}+{:x})", i, frames[i], nm, disp, base, hm ? (uintptr_t)frames[i] - (uintptr_t)hm : 0);
    }
    if (g_curThread)
        DumpContext(g_curThread->ctx);
    fflush(stdout);
    extern bool g_noMessageBox;
    if (!g_noMessageBox) MessageBoxA(nullptr, "The game crashed. See BlackKnightRecomp.log for details.", "BlackKnightRecomp", MB_ICONERROR);
    return 0;
}

void Crash_Init()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    BuildMap();
    AddVectoredExceptionHandler(1, CrashHandler);
}

// ---------------------------------------------------------------- watchdog / sampling profiler
#include <thread>
static HANDLE s_guestThread = nullptr;
extern uint32_t g_framesPresented;

static void WatchdogThread()
{
    uint32_t lastFrames = 0;
    for (;;)
    {
        Sleep(3000);
        if (!s_guestThread) continue;
        // sample a few times and report the most frequent guest functions
        uint32_t samples[16];
        for (int i = 0; i < 16; i++)
        {
            SuspendThread(s_guestThread);
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            GetThreadContext(s_guestThread, &c);
            ResumeThread(s_guestThread);
            samples[i] = Crash_GuestFunctionForHost((uintptr_t)c.Rip);
            Sleep(5);
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
            LARGE_INTEGER q, fq;
            QueryPerformanceCounter(&q);
            QueryPerformanceFrequency(&fq);
            double secs = lastQpc ? (double)(q.QuadPart - lastQpc) / fq.QuadPart : 3.0;
            lastQpc = q.QuadPart;
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
// for more than 3 ms (finds code paths that starve interrupt delivery).
static std::string Crash_SymbolName(uintptr_t rip)
{
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 disp = 0;
    return SymFromAddr(GetCurrentProcess(), rip, &disp, si) ? std::string(si->Name) : std::format("?{:x}", rip);
}

static void IrqStallSampler()
{
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    extern std::atomic<uint64_t> g_lastIrqCheckTb;
    std::map<std::string, int> where;
    int total = 0;
    timeBeginPeriod(1);
    for (int iter = 0;; iter++)
    {
        Sleep(1);
        uint64_t last = g_lastIrqCheckTb.load(std::memory_order_relaxed);
        if (last && Timebase_Now() - last > TB_FREQ * 3 / 1000)
        {
            SuspendThread(s_guestThread);
            CONTEXT c{};
            c.ContextFlags = CONTEXT_FULL;
            GetThreadContext(s_guestThread, &c);
            std::string chain;
            for (int i = 0; i < 6 && c.Rip; i++)
            {
                std::string n = Crash_SymbolName((uintptr_t)c.Rip);
                chain += n + " < ";
                DWORD64 imageBase;
                PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
                if (!rf) break;
                void* hd;
                DWORD64 ef;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, rf, &c, &hd, &ef, nullptr);
            }
            ResumeThread(s_guestThread);
            where[chain]++;
            total++;
        }
        if (iter % 20000 == 19999 && total)
        {
            std::vector<std::pair<int, std::string>> v;
            for (auto& [k, n] : where) v.push_back({ n, k });
            std::sort(v.rbegin(), v.rend());
            LOG_INFO("=== interrupt stalls (>3 ms without polling): {} samples ===", total);
            for (size_t i = 0; i < v.size() && i < 12; i++) LOG_INFO("  {:4}  {}", v[i].first, v[i].second);
        }
    }
}

// BKR_SLOW_SAMPLE=1: the scheduler's timer thread calls this when the game thread has
// not reacted to a due event for >3 ms (it is awake anyway, so timing is unchanged).
// Records where the game thread is; logged every 30 s.
static std::mutex s_slowMutex;
static std::map<std::string, int> s_slowWhere;
void Crash_SampleSlowGuest()
{
    if (!s_guestThread) return;
    static bool symInit = false;
    if (!symInit) { symInit = true; SymInitialize(GetCurrentProcess(), nullptr, TRUE); }
    SuspendThread(s_guestThread);
    CONTEXT c{};
    c.ContextFlags = CONTEXT_FULL;
    GetThreadContext(s_guestThread, &c);
    uintptr_t frames[10];
    int nf = 0;
    for (int i = 0; i < 10 && c.Rip; i++)
    {
        frames[nf++] = (uintptr_t)c.Rip;
        DWORD64 imageBase;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (!rf) break;
        void* hd;
        DWORD64 ef;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, rf, &c, &hd, &ef, nullptr);
    }
    ResumeThread(s_guestThread);
    std::string chain;
    for (int i = 0; i < nf; i++) chain += Crash_SymbolName(frames[i]) + " < ";
    std::lock_guard lk(s_slowMutex);
    s_slowWhere[chain]++;
}

static void LogSlowGuest()
{
    std::lock_guard lk(s_slowMutex);
    if (s_slowWhere.empty()) return;
    std::vector<std::pair<int, std::string>> v;
    for (auto& [k, n] : s_slowWhere) v.push_back({ n, k });
    std::sort(v.rbegin(), v.rend());
    LOG_INFO("=== game thread slow to take interrupts (>3 ms): where ===");
    for (size_t i = 0; i < v.size() && i < 10; i++) LOG_INFO("  {:4}  {}", v[i].first, v[i].second);
    s_slowWhere.clear();
}

void Crash_StartWatchdog()
{
    if (getenv("BKR_SLOW_SAMPLE"))
        std::thread([] { for (;;) { Sleep(30000); LogSlowGuest(); } }).detach();
    if (getenv("BKR_IRQ_STALLS")) std::thread(IrqStallSampler).detach();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_guestThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    std::thread(WatchdogThread).detach();
    // BKR_TEST_PAUSE=start,end (seconds): exercises the focus-loss pause without
    // touching window focus
    if (const char* tp = getenv("BKR_TEST_PAUSE"))
    {
        int a = 0, b = 0;
        if (sscanf(tp, "%d,%d", &a, &b) == 2 && b > a)
            std::thread([a, b] {
                extern std::atomic<bool> g_pauseRequested;
                Sleep(a * 1000);
                g_pauseRequested = true;
                Sleep((b - a) * 1000);
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

// ---------------------------------------------------------------- sampling profiler
#include <map>
#include <set>
HANDLE g_gxThreadHandle = nullptr;
bool g_profileGX = false;   // --profile-gx: sample the GX thread instead of the CPU thread

static void ProfilerThread(int startSec, int durationSec)
{
    Sleep(startSec * 1000);
    HANDLE target = (g_profileGX && g_gxThreadHandle) ? g_gxThreadHandle : s_guestThread;
    std::map<uintptr_t, int> hits, stacks;
    std::map<uint32_t, int> interpHits;   // guest PC (64-byte buckets) seen by the interpreter
    std::vector<std::vector<uintptr_t>> stackSamples;
    int total = 0;
    timeBeginPeriod(1);
    DWORD end = GetTickCount() + durationSec * 1000;
    while (GetTickCount() < end)
    {
        SuspendThread(target);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        GetThreadContext(target, &c);
        hits[(uintptr_t)c.Rip]++;
        { extern volatile uint32_t g_interpPC; interpHits[g_interpPC & ~0x3Fu]++; }
        // walk up to 24 frames for inclusive attribution
        uintptr_t frames[24];
        int nf = 0;
        for (int i = 0; i < 24 && c.Rip; i++)
        {
            frames[nf++] = (uintptr_t)c.Rip;
            DWORD64 imageBase;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
            if (!rf) break;
            void* hd;
            DWORD64 ef;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, rf, &c, &hd, &ef, nullptr);
        }
        ResumeThread(target);
        for (int i = 0; i < nf; i++) stacks[frames[i]]++;
        stackSamples.push_back(std::vector<uintptr_t>(frames, frames + nf));
        total++;
        Sleep(1);
    }
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    std::map<std::string, int> byFunc;
    for (auto& [rip, n] : hits)
    {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        std::string name = SymFromAddr(GetCurrentProcess(), rip, &disp, si) ? si->Name : std::format("?{:x}", rip >> 12 << 12);
        byFunc[name] += n;
    }
    std::vector<std::pair<int, std::string>> v;
    for (auto& [k, n] : byFunc) v.push_back({ n, k });
    std::sort(v.rbegin(), v.rend());
    LOG_INFO("=== profile: {} samples ===", total);
    for (size_t i = 0; i < v.size() && i < 40; i++)
        LOG_INFO("  {:5.1f}%  {}", 100.0 * v[i].first / total, v[i].second);
    // inclusive: count each function once per sample
    std::map<uintptr_t, std::string> names;
    auto nameOf = [&](uintptr_t rip) -> const std::string& {
        auto it = names.find(rip);
        if (it != names.end()) return it->second;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        return names[rip] = SymFromAddr(GetCurrentProcess(), rip, &disp, si) ? si->Name : std::format("?{:x}", rip >> 12 << 12);
    };
    std::map<std::string, int> incl;
    for (auto& st : stackSamples)
    {
        std::set<std::string> seen;
        for (uintptr_t r : st) seen.insert(nameOf(r));
        for (auto& n : seen) incl[n]++;
    }
    std::vector<std::pair<int, std::string>> vi;
    for (auto& [k, n] : incl) if (k.rfind("fn_", 0) != 0) vi.push_back({ n, k });
    std::sort(vi.rbegin(), vi.rend());
    LOG_INFO("=== inclusive (host functions) ===");
    for (size_t i = 0; i < vi.size() && i < 50; i++)
        LOG_INFO("  {:5.1f}%  {}", 100.0 * vi[i].first / total, vi[i].second);
    // callers of the wait
    std::map<std::string, int> waitCallers;
    for (auto& st : stackSamples)
    {
        if (st.empty() || nameOf(st[0]).find("Wait") == std::string::npos) continue;
        std::string chain;
        for (size_t i = 1, k = 0; i < st.size() && k < 5; i++)
        {
            const std::string& n = nameOf(st[i]);
            if (n.find("Wait") != std::string::npos) continue;
            chain += n + " < ";
            k++;
        }
        waitCallers[chain]++;
    }
    std::vector<std::pair<int, std::string>> vw;
    for (auto& [k, n] : waitCallers) vw.push_back({ n, k });
    std::sort(vw.rbegin(), vw.rend());
    {
        std::vector<std::pair<int, uint32_t>> vh;
        for (auto& [k, n] : interpHits) if (k) vh.push_back({ n, k });
        std::sort(vh.rbegin(), vh.rend());
        LOG_INFO("=== interpreter guest PCs (last seen, 64-byte buckets) ===");
        for (size_t i = 0; i < vh.size() && i < 15; i++) LOG_INFO("  {:5.1f}%  {:08x}", 100.0 * vh[i].first / total, vh[i].second);
    }
    LOG_INFO("=== wait call chains ===");
    for (size_t i = 0; i < vw.size() && i < 12; i++)
        LOG_INFO("  {:5.1f}%  {}", 100.0 * vw[i].first / total, vw[i].second);
}

void Crash_StartProfiler(int startSec, int durationSec)
{
    std::thread(ProfilerThread, startSec, durationSec).detach();
}
