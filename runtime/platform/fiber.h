#pragma once
// Cooperative host fibers for guest threads (kernel/threads.cpp). A fiber saves the
// callee-saved registers and the floating point control state when it switches away.
#include <cstddef>

typedef void (*FiberProc)(void* arg);

// Turns the calling thread into a fiber (the first fiber of that thread).
void* Fiber_FromThread();
// A new fiber with `stackSize` bytes of (lazily committed) stack; fn must never return.
void* Fiber_Create(size_t stackSize, FiberProc fn, void* arg);
// Switches the calling thread to `to`; returns when some fiber switches back to us.
void Fiber_Switch(void* to);
// Frees a fiber that is not running.
void Fiber_Delete(void* fiber);
