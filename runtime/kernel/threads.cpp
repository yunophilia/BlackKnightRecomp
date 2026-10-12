// Guest threads on host fibers + exception (interrupt) delivery.
#include "threads.h"
#include "memory.h"
#include "scheduler.h"
#include "../cpu/cpu.h"
#include "../hw/hw.h"
#include "../log.h"
#include "../../generated/ppc_recomp_shared.h"

#include <atomic>
#include <unordered_map>
#include <vector>
#include <windows.h>

GuestThread* g_curThread = nullptr;
static GuestThread s_mainThread;
static std::unordered_map<uint32_t, GuestThread*> s_byContext;
static std::vector<GuestThread*> s_graveyard;

// Addresses in this executable (RENE8P)
static constexpr uint32_t ADDR_OS_CURRENT_THREAD = 0x800000E4;
static constexpr uint32_t ADDR_OS_CURRENT_CONTEXT = 0x800000D4;
static constexpr uint32_t ADDR_EXCEPTION_TABLE_PTR = 0x80805028;   // r13 - 5784

static constexpr SIZE_T FIBER_STACK = 256 * 1024 * 1024;

// ------------------------------------------------------------- context helpers
static void LoadCtxFromGuest(PPCContext& c, uint32_t oc)
{
    for (int i = 0; i < 32; i++) c.r[i] = Read32(oc + 4 * i);
    ppc_set_cr(c, Read32(oc + 0x80));
    c.lr = Read32(oc + 0x84);
    c.ctr = Read32(oc + 0x88);
    ppc_set_xer(c, Read32(oc + 0x8C));
    for (int i = 0; i < 32; i++) c.f[i].u64 = Read64(oc + 0x90 + 8 * i);
    c.fpscr = Read32(oc + 0x194);
    c.srr0 = Read32(oc + 0x198);
    c.srr1 = Read32(oc + 0x19C);
    for (int i = 0; i < 8; i++) c.gqr[i] = Read32(oc + 0x1A4 + 4 * i);
    for (int i = 0; i < 32; i++)
    {
        uint64_t b = Read64(oc + 0x1C8 + 8 * i);
        memcpy(&c.f[i].ps1, &b, 8);
    }
    c.msr = c.srr1;
}

static void SaveCtxToGuest(const PPCContext& c, uint32_t oc, uint32_t srr0)
{
    for (int i = 0; i < 32; i++) Write32(oc + 4 * i, c.r[i]);
    Write32(oc + 0x80, ppc_get_cr(const_cast<PPCContext&>(c)));
    Write32(oc + 0x84, c.lr);
    Write32(oc + 0x88, c.ctr);
    Write32(oc + 0x8C, ppc_get_xer(const_cast<PPCContext&>(c)));
    Write32(oc + 0x198, srr0);
    Write32(oc + 0x19C, c.msr);
}

// ------------------------------------------------------------- fibers
extern bool g_irqDeferred;
static void SwitchTo(GuestThread* t)
{
    // the resumed thread may run with interrupts enabled: let it see deferred ones
    if (g_irqDeferred) g_ppcIrqPending = 1;
    GuestThread* self = g_curThread;
    g_curThread = t;
    SwitchToFiber(t->fiber);
    // resumed: whoever switched to us set g_curThread = self
    g_curThread = self;
    for (GuestThread* d : s_graveyard)
    {
        if (d != self && d->fiber)
        {
            DeleteFiber(d->fiber);
            delete d;
        }
    }
    s_graveyard.erase(std::remove_if(s_graveyard.begin(), s_graveyard.end(), [&](GuestThread* d) { return d != self; }), s_graveyard.end());
}

static void WINAPI ThreadFiberProc(void* param)
{
    GuestThread* t = (GuestThread*)param;
    g_curThread = t;
    LOG_INFO("guest thread start: ctx={:08x} entry={:08x} sp={:08x}", t->osContext, t->entry, t->ctx.r[1]);
    t->ctx.lr = t->exitLr;
    // entry may be outside the recompiled image (e.g. the HOME menu RSO module),
    // in which case the interpreter runs it
    ppc_call_indirect(t->ctx, g_base, t->entry);
    PPCFunc ex = ppc_lookup(t->exitLr);
    if (ex) ex(t->ctx, g_base);
    FATAL("guest thread {:08x} returned past its exit function", t->osContext);
}

static GuestThread* CreateFromContext(uint32_t oc)
{
    GuestThread* t = new GuestThread();
    LoadCtxFromGuest(t->ctx, oc);
    t->osContext = oc;
    t->entry = t->ctx.srr0;
    t->exitLr = t->ctx.lr;
    t->fiber = CreateFiberEx(64 * 1024, FIBER_STACK, FIBER_FLAG_FLOAT_SWITCH, ThreadFiberProc, t);
    if (!t->fiber) FATAL("CreateFiberEx failed ({})", GetLastError());
    s_byContext[oc] = t;
    return t;
}

// ------------------------------------------------------------- HLE: context primitives
// OSSaveContext(OSContext*) -> 0 now; "returns 1" later via fiber resumption.
PPC_FUNC(fn_8053DAB0)
{
    uint32_t oc = ctx.r[3];
    fn_8053DAB0_impl(ctx, base);
    auto it = s_byContext.find(oc);
    if (it != s_byContext.end() && it->second != g_curThread)
    {
        // context re-bound to this fiber
        it->second->osContext = 0;
    }
    s_byContext[oc] = g_curThread;
    g_curThread->osContext = oc;
}

// OSLoadContext(OSContext*) -- never returns on hardware.
PPC_FUNC(fn_8053DB30)
{
    uint32_t oc = ctx.r[3];
    GuestThread* self = g_curThread;
    if (self->irqDepth > 0 && oc == self->irqContext[self->irqDepth - 1])
        longjmp(*self->irqJmp[self->irqDepth - 1], 1);

    GuestThread* target;
    auto it = s_byContext.find(oc);
    if (it != s_byContext.end())
        target = it->second;
    else
        target = CreateFromContext(oc);

    if (target == self)
    {
        // Resuming our own saved context: the caller (SelectThread) simply returns.
        return;
    }
    SwitchTo(target);
}

// OSInitContext(OSContext*, pc, sp): a context is being (re)initialised for a new thread.
PPC_FUNC(fn_8053DC94)
{
    uint32_t oc = ctx.r[3];
    auto it = s_byContext.find(oc);
    if (it != s_byContext.end())
    {
        GuestThread* old = it->second;
        s_byContext.erase(it);
        if (old != g_curThread && old != &s_mainThread)
        {
            old->dead = true;
            s_graveyard.push_back(old);
        }
    }
    fn_8053DC94_impl(ctx, base);
}

// OSEnableInterrupts: when called with no current thread we are in the scheduler's
// idle loop -- block the host thread until something can wake the guest.
PPC_FUNC(fn_80541740)
{
    if (Read32(ADDR_OS_CURRENT_THREAD) == 0)
        Sched_IdleWait();
    fn_80541740_impl(ctx, base);
}

// ------------------------------------------------------------- exceptions
uint32_t g_excCount[16];
static void TakeException(PPCContext& ctx, uint8_t* base, uint32_t exc)
{
    g_excCount[exc & 15]++;
    GuestThread* t = g_curThread;
    uint32_t table = Read32(ADDR_EXCEPTION_TABLE_PTR);
    uint32_t handler = table ? Read32(table + 4 * exc) : 0;
    uint32_t context = Read32(ADDR_OS_CURRENT_CONTEXT);
    if (!handler || !context)
        return;
    if (t->irqDepth >= 8)
        FATAL("exception nesting too deep");

    PPCContext saved = ctx;
    SaveCtxToGuest(ctx, context, ctx.lr);
    Write16(context + 0x1A2, Read16(context + 0x1A2) | 2);

    jmp_buf jb;
    int depth = t->irqDepth++;
    t->irqContext[depth] = context;
    t->irqJmp[depth] = &jb;
    if (setjmp(jb) == 0)
    {
        ctx.srr0 = saved.lr;
        ctx.srr1 = saved.msr;
        ctx.msr &= ~(MSR_EE | 0x4000 /*PR*/ | 0x0400 /*SE*/ | 0x0100 /*FE1*/ | 0x0800 /*FE0*/);
        ctx.r[3] = exc;
        ctx.r[4] = context;
        PPCFunc f = ppc_lookup(handler);
        if (!f) FATAL("exception handler {:08x} unknown", handler);
        f(ctx, base);
    }
    g_curThread->irqDepth = depth;
    ctx = saved;
}

void Cpu_PauseIfRequested();
std::atomic<uint64_t> g_lastIrqCheckTb{ 0 };   // last interrupt poll (BKR_IRQ_STALLS diagnostics)
bool g_irqDeferred = false;   // an interrupt arrived while MSR[EE] was off
extern "C" void ppc_check_irq(PPCContext& ctx, uint8_t* base)
{
    g_lastIrqCheckTb.store(Timebase_Now(), std::memory_order_relaxed);
    Cpu_PauseIfRequested();
    for (;;)
    {
        g_ppcIrqPending = 0;
        Sched_RunDue();
        if (!(ctx.msr & MSR_EE))
        {
            // Interrupts are disabled: remember it and stop polling on every loop
            // back-edge (a cache-flush loop inside a disc-completion handler spent
            // milliseconds here, delaying audio). Re-enabling interrupts (mtmsr) or a
            // thread switch delivers it; the timer thread re-raises for new events.
            if (Dec_Expired() || HW_PIInterruptPending())
                g_irqDeferred = true;
            return;
        }
        if (HW_PIInterruptPending())
        {
            TakeException(ctx, base, EXC_EXTERNAL);
            continue;
        }
        if (Dec_Expired())
        {
            Dec_Ack();
            TakeException(ctx, base, EXC_DECREMENTER);
            continue;
        }
        return;
    }
}

void Irq_Update()
{
    if (HW_PIInterruptPending() || Dec_Expired())
    {
        g_ppcIrqPending = 1;
        Sched_WakeIdle();
    }
}

// ------------------------------------------------------------- boot
void Threads_Init()
{
    s_mainThread.fiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    strcpy_s(s_mainThread.name, "main");
    g_curThread = &s_mainThread;
}

void Threads_RunMain(uint32_t entry)
{
    PPCContext& ctx = s_mainThread.ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.msr = 0x00002032;   // FP | IR | DR | RI
    ctx.r[1] = 0x816FFFF0;
    ctx.hid0 = 0x0011C464;
    ctx.hid2 = 0xE0000000;  // PSE | WPE (paired singles, write gather) | LCE
    ctx.hid4 = 0x83900000;
    PPCFunc f = ppc_lookup(entry);
    if (!f) FATAL("entry point {:08x} not found", entry);
    LOG_INFO("jumping to entry point {:08x}", entry);
    f(ctx, g_base);
    FATAL("entry point returned");
}
