// Guest thread fibers: Windows fibers.
#include "../fiber.h"

#include <windows.h>

void* Fiber_FromThread() { return ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH); }

void* Fiber_Create(size_t stackSize, FiberProc fn, void* arg)
{
    // 64 KiB committed up front, the rest on demand
    return CreateFiberEx(64 * 1024, stackSize, FIBER_FLAG_FLOAT_SWITCH, (LPFIBER_START_ROUTINE)fn, arg);
}

void Fiber_Switch(void* to) { SwitchToFiber(to); }

void Fiber_Delete(void* fiber) { DeleteFiber(fiber); }
