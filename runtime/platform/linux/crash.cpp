// Crash reports on Linux: SIGSEGV/SIGBUS/SIGILL/SIGFPE handler with a host backtrace,
// and the watchdog's guest thread sampling (a signal that reads the interrupted pc).
// The Windows-only debugging aids (--watch, --profile, BKR_SLOW_SAMPLE, BKR_IRQ_STALLS)
// are not available here.
#include "../../kernel/crash.h"
#include "../../kernel/memory.h"
#include "../../kernel/threads.h"
#include "../../log.h"
#include "../platform.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <ucontext.h>
#include <unistd.h>

extern bool g_noMessageBox;
void Gamepad_StopRumble();

static std::atomic<bool> s_guestKnown{ false };
static pthread_t s_guestThread;
static std::atomic<uintptr_t> s_sampledPC{ 0 };
static std::atomic<bool> s_sampled{ false };
static const int SIG_SAMPLE = SIGRTMIN + 3;

static uintptr_t ContextPC(void* uc) { return (uintptr_t)((ucontext_t*)uc)->uc_mcontext.gregs[REG_RIP]; }

static const char* SignalName(int sig)
{
    switch (sig)
    {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    default: return "signal";
    }
}

static void CrashHandler(int sig, siginfo_t* si, void* uc)
{
    static std::atomic<bool> inHandler{ false };
    if (inHandler.exchange(true)) _exit(2);
    uintptr_t rip = ContextPC(uc);
    uintptr_t addr = (sig == SIGSEGV || sig == SIGBUS) ? (uintptr_t)si->si_addr : 0;
    LOG_ERROR("=== host exception {} (code {}) at rip={:x} (fault addr {:x}) ===", SignalName(sig), si->si_code, rip, addr);
    if (g_base && addr >= (uintptr_t)g_base && addr < (uintptr_t)g_base + 0x100000000ull)
        LOG_ERROR("  guest address {:08x}", (uint32_t)(addr - (uintptr_t)g_base));
    uint32_t gf = Crash_GuestFunctionForHost(rip);
    LOG_ERROR("  in guest function ~fn_{:08X} (host offset +{:x})", gf, gf ? rip - Crash_FirstHostFunction() : 0);
    void* frames[62];
    int n = backtrace(frames, 62);
    for (int i = 0; i < n; i++)
    {
        Dl_info di{};
        bool ok = dladdr(frames[i], &di) != 0;
        const char* mod = ok && di.dli_fname ? di.dli_fname : "?";
        if (const char* slash = strrchr(mod, '/')) mod = slash + 1;
        uintptr_t symOff = ok && di.dli_saddr ? (uintptr_t)frames[i] - (uintptr_t)di.dli_saddr : 0;
        uintptr_t modOff = ok && di.dli_fbase ? (uintptr_t)frames[i] - (uintptr_t)di.dli_fbase : 0;
        LOG_ERROR("  host frame {:2}: {} {}+{:x} ({}+{:x})", i, frames[i], ok && di.dli_sname ? di.dli_sname : "?", symOff, mod, modOff);
    }
    if (g_curThread) Crash_DumpContext(g_curThread->ctx);
    fflush(stdout);
    Gamepad_StopRumble();
    if (!g_noMessageBox) Platform_ErrorBox("BlackKnightRecomp", "The game crashed. See BlackKnightRecomp.log for details.");
    _exit(2);
}

// Each thread that can fault on a full stack needs its own signal stack.
static void AddSignalStack()
{
    stack_t ss{};
    ss.ss_size = 1 << 20;
    ss.ss_sp = malloc(ss.ss_size);
    sigaltstack(&ss, nullptr);
}

static void SampleHandler(int, siginfo_t*, void* uc)
{
    s_sampledPC.store(ContextPC(uc), std::memory_order_relaxed);
    s_sampled.store(true, std::memory_order_release);
}

void Crash_Init()
{
    Crash_BuildMap();
    AddSignalStack();
    struct sigaction sa{};
    sa.sa_sigaction = CrashHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE }) sigaction(sig, &sa, nullptr);
    struct sigaction ss{};
    ss.sa_sigaction = SampleHandler;
    ss.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&ss.sa_mask);
    sigaction(SIG_SAMPLE, &ss, nullptr);
}

void Crash_StartSamplers()
{
    if (getenv("BKR_SLOW_SAMPLE") || getenv("BKR_IRQ_STALLS")) LOG_WARN("BKR_SLOW_SAMPLE / BKR_IRQ_STALLS are not available on Linux");
    s_guestThread = pthread_self();
    s_guestKnown = true;
}

bool Crash_GuestThreadKnown() { return s_guestKnown; }

uintptr_t Crash_SampleGuestPC()
{
    s_sampled.store(false, std::memory_order_relaxed);
    if (pthread_kill(s_guestThread, SIG_SAMPLE) != 0) return 0;
    for (int i = 0; i < 1000 && !s_sampled.load(std::memory_order_acquire); i++) usleep(50);
    return s_sampled.load(std::memory_order_acquire) ? s_sampledPC.load(std::memory_order_relaxed) : 0;
}

void Crash_SetGxThread() { AddSignalStack(); }

void Crash_SetWatch(uint32_t) { LOG_WARN("--watch is not available on Linux"); }

void Crash_StartProfiler(int, int) { LOG_WARN("--profile is not available on Linux"); }

void Crash_SampleSlowGuest() {}
bool g_profileGX = false;   // --profile-gx (Windows profiler only)
