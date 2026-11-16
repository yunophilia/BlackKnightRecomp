// Crash reports and sampling profilers on Windows: vectored exception handler, dbghelp
// symbols and RtlVirtualUnwind stack walks.
#include "../../kernel/crash.h"
#include "../../kernel/threads.h"
#include "../../kernel/memory.h"
#include "../../log.h"
#include "../../cpu/cpu.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#pragma comment(lib, "dbghelp.lib")

static HANDLE s_guestThread = nullptr;

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
    LOG_ERROR("  in guest function ~fn_{:08X} (host offset +{:x})", gf, gf ? rip - (uintptr_t)Crash_FirstHostFunction() : 0);
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
        Crash_DumpContext(g_curThread->ctx);
    fflush(stdout);
    extern bool g_noMessageBox;
    if (!g_noMessageBox) MessageBoxA(nullptr, "The game crashed. See BlackKnightRecomp.log for details.", "BlackKnightRecomp", MB_ICONERROR);
    return 0;
}

void Crash_Init()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Crash_BuildMap();
    AddVectoredExceptionHandler(1, CrashHandler);
}

// ---------------------------------------------------------------- guest thread sampling
bool Crash_GuestThreadKnown() { return s_guestThread != nullptr; }

uintptr_t Crash_SampleGuestPC()
{
    SuspendThread(s_guestThread);
    CONTEXT c{};
    c.ContextFlags = CONTEXT_CONTROL;
    GetThreadContext(s_guestThread, &c);
    ResumeThread(s_guestThread);
    return (uintptr_t)c.Rip;
}

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

void Crash_StartSamplers()
{
    if (getenv("BKR_SLOW_SAMPLE"))
        std::thread([] { for (;;) { Sleep(30000); LogSlowGuest(); } }).detach();
    if (getenv("BKR_IRQ_STALLS")) std::thread(IrqStallSampler).detach();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_guestThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
}

// ---------------------------------------------------------------- sampling profiler
static HANDLE g_gxThreadHandle = nullptr;
bool g_profileGX = false;   // --profile-gx: sample the GX thread instead of the CPU thread

void Crash_SetGxThread()
{
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_gxThreadHandle, 0, FALSE, DUPLICATE_SAME_ACCESS);
}

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
