// Guest thread fibers: a small x86-64 context switch (System V ABI). Only the callee-saved
// registers, MXCSR and the x87 control word need saving, because a switch is an ordinary
// function call for the code around it. ucontext would also work, but swapcontext makes
// a system call (signal mask) on every switch.
#include "../fiber.h"
#include "../../log.h"

#include <sys/mman.h>
#include <unistd.h>

namespace
{
struct Fiber
{
    void* sp = nullptr;          // saved stack pointer while not running
    void* stack = nullptr;       // mapping (guard page first), null for a thread's fiber
    size_t mapped = 0;
    FiberProc fn = nullptr;
    void* arg = nullptr;
};

thread_local Fiber* t_current = nullptr;
}   // namespace

// bkr_fiber_switch(void** saveSp, void* newSp): saves the current context on its stack,
// stores the stack pointer to *saveSp and resumes the context saved at newSp.
// bkr_fiber_start: first "return address" of a new fiber; r12 = entry, r13 = fiber.
asm(R"(
    .text
    .globl bkr_fiber_switch
    .type bkr_fiber_switch, @function
bkr_fiber_switch:
    .cfi_startproc
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8, %rsp
    stmxcsr (%rsp)
    fnstcw 4(%rsp)
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    ldmxcsr (%rsp)
    fldcw 4(%rsp)
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
    .cfi_endproc
    .size bkr_fiber_switch, .-bkr_fiber_switch

    .globl bkr_fiber_start
    .type bkr_fiber_start, @function
bkr_fiber_start:
    .cfi_startproc
    .cfi_undefined rip
    movq %r13, %rdi
    callq *%r12
    ud2
    .cfi_endproc
    .size bkr_fiber_start, .-bkr_fiber_start
)");

extern "C" void bkr_fiber_switch(void** saveSp, void* newSp);
extern "C" void bkr_fiber_start();

static void FiberEntry(Fiber* f)
{
    f->fn(f->arg);
    FATAL("a guest thread fiber returned");
}

void* Fiber_FromThread()
{
    if (!t_current) t_current = new Fiber();
    return t_current;
}

void* Fiber_Create(size_t stackSize, FiberProc fn, void* arg)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t size = (stackSize + page - 1) / page * page + page;
    // reserved, committed as it is touched (like the Windows fiber stacks)
    void* m = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (m == MAP_FAILED) return nullptr;
    mprotect(m, page, PROT_NONE);   // guard page: overflowing the stack faults
    Fiber* f = new Fiber();
    f->stack = m;
    f->mapped = size;
    f->fn = fn;
    f->arg = arg;
    // initial frame as bkr_fiber_switch leaves it: control word slot, r15..rbp, return
    uintptr_t top = ((uintptr_t)m + size) & ~(uintptr_t)15;
    uint64_t* sp = (uint64_t*)top;
    *--sp = (uint64_t)&bkr_fiber_start;   // popped by ret: bkr_fiber_start runs with rsp = top (aligned)
    *--sp = 0;                          // rbp
    *--sp = 0;                          // rbx
    *--sp = (uint64_t)&FiberEntry;      // r12
    *--sp = (uint64_t)f;                // r13
    *--sp = 0;                          // r14
    *--sp = 0;                          // r15
    uint32_t mxcsr;
    uint16_t fcw;
    asm volatile("stmxcsr %0" : "=m"(mxcsr));
    asm volatile("fnstcw %0" : "=m"(fcw));
    *--sp = (uint64_t)mxcsr | ((uint64_t)fcw << 32);
    f->sp = sp;
    return f;
}

void Fiber_Switch(void* to)
{
    Fiber* self = t_current;
    Fiber* target = (Fiber*)to;
    if (target == self) return;
    t_current = target;
    bkr_fiber_switch(&self->sp, target->sp);
    t_current = self;
}

void Fiber_Delete(void* fiber)
{
    Fiber* f = (Fiber*)fiber;
    if (f->stack) munmap(f->stack, f->mapped);
    delete f;
}
