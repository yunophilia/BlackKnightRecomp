#pragma once
#include "../cpu/ppc_context.h"
#include <csetjmp>

// Each guest OS thread runs on its own host fiber with its own PPCContext.
// The game's own scheduler (recompiled) decides what runs; we only implement the
// CPU-level context switch (OSSaveContext/OSLoadContext) and exception entry.
struct GuestThread
{
    void* fiber = nullptr;
    PPCContext ctx{};
    uint32_t osContext = 0;      // guest OSContext this fiber is bound to
    uint32_t entry = 0;          // initial pc (for new threads)
    uint32_t exitLr = 0;
    // exception (interrupt) nesting state
    int irqDepth = 0;
    uint32_t irqContext[8] = {};
    jmp_buf* irqJmp[8] = {};
    bool dead = false;
    char name[32] = {};
};

extern GuestThread* g_curThread;

void Threads_Init();
[[noreturn]] void Threads_RunMain(uint32_t entry);

// Exceptions
enum : uint32_t
{
    EXC_EXTERNAL = 4,
    EXC_DECREMENTER = 8,
};

// Wake the scheduler/timer thread because a deadline changed.
void Sched_Kick();
// Raised by hardware when PI interrupt state changes.
void Irq_Update();
