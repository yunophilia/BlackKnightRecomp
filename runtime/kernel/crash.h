#pragma once
// Crash reports, the watchdog and the sampling profilers (kernel/crash.cpp, and the
// platform parts in platform/*/crash.cpp).
#include "../cpu/ppc_context.h"
#include <cstdint>

void Crash_Init();                      // installs the fault handler (platform)
void Crash_BuildMap();                  // host code address -> guest function table
uint32_t Crash_GuestFunctionForHost(uintptr_t rip);
uintptr_t Crash_FirstHostFunction();    // lowest recompiled function (offsets in reports)
void Crash_DumpContext(const PPCContext& c);
void Crash_DumpThreads();
void Crash_StartWatchdog();

// platform
void Crash_StartSamplers();             // BKR_SLOW_SAMPLE/BKR_IRQ_STALLS; remembers the guest thread
bool Crash_GuestThreadKnown();
uintptr_t Crash_SampleGuestPC();        // where the guest thread is right now
void Crash_SetGxThread();               // called on the GX thread (--profile-gx)
void Crash_SetWatch(uint32_t addr);     // --watch: log writes to a guest page
void Crash_StartProfiler(int startSec, int durationSec);
