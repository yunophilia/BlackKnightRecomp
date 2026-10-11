#include "memory.h"
#include "../log.h"

#include <windows.h>

uint8_t* g_base = nullptr;

typedef PVOID(WINAPI* VirtualAlloc2_t)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
typedef PVOID(WINAPI* MapViewOfFile3_t)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);

// Reserve a 4 GiB placeholder and map the guest RAM views into it so that
// cached/uncached mirrors alias the same physical pages.
bool Memory_Init()
{
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    auto pVirtualAlloc2 = (VirtualAlloc2_t)GetProcAddress(kb, "VirtualAlloc2");
    auto pMapViewOfFile3 = (MapViewOfFile3_t)GetProcAddress(kb, "MapViewOfFile3");
    if (!pVirtualAlloc2 || !pMapViewOfFile3)
    {
        LOG_ERROR("VirtualAlloc2/MapViewOfFile3 unavailable (need Windows 10 1803+)");
        return false;
    }

    const uint64_t total = 0x100000000ull;
    uint8_t* base = (uint8_t*)pVirtualAlloc2(nullptr, nullptr, total, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    if (!base)
    {
        LOG_ERROR("failed to reserve guest address space ({})", GetLastError());
        return false;
    }

    struct Region { uint32_t addr; uint32_t size; HANDLE section; };
    HANDLE mem1 = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, MEM1_SIZE, nullptr);
    HANDLE mem2 = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, MEM2_SIZE, nullptr);
    HANDLE lc = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 0x10000, nullptr);
    Region regions[] = {
        { 0x00000000, MEM1_SIZE, mem1 },   // physical (translation off) aliases
        { 0x10000000, MEM2_SIZE, mem2 },
        { 0x80000000, MEM1_SIZE, mem1 },
        { 0x90000000, MEM2_SIZE, mem2 },
        { 0xC0000000, MEM1_SIZE, mem1 },
        { 0xD0000000, MEM2_SIZE, mem2 },
        { LOCKED_CACHE_ADDR, 0x10000, lc },
    };

    // Split the placeholder so each region is its own placeholder.
    for (auto& r : regions)
    {
        uint8_t* p = base + r.addr;
        if (!VirtualFree(p, r.size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
        {
            LOG_ERROR("placeholder split failed at {:08x} ({})", r.addr, GetLastError());
            return false;
        }
    }
    for (auto& r : regions)
    {
        void* v = pMapViewOfFile3(r.section, GetCurrentProcess(), base + r.addr, 0, r.size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        if (v != base + r.addr)
        {
            LOG_ERROR("map view failed at {:08x} ({})", r.addr, GetLastError());
            return false;
        }
    }
    g_base = base;
    LOG_INFO("guest memory at {}", (void*)base);
    return true;
}
