#pragma once
#include <cstdint>
#include <cstring>
#include "../platform/compat.h"

// Guest physical memory layout (Wii)
constexpr uint32_t MEM1_SIZE = 0x01800000;   // 24 MiB
constexpr uint32_t MEM2_SIZE = 0x04000000;   // 64 MiB
constexpr uint32_t LOCKED_CACHE_ADDR = 0xE0000000;
constexpr uint32_t LOCKED_CACHE_SIZE = 0x4000;

extern uint8_t* g_base;   // host address of guest address 0

bool Memory_Init();

inline uint8_t* GuestPtr(uint32_t addr) { return g_base + addr; }
template <typename T> inline T* GuestPtrT(uint32_t addr) { return (T*)(g_base + addr); }

inline uint8_t Read8(uint32_t a) { return g_base[a]; }
inline uint16_t Read16(uint32_t a) { return _byteswap_ushort(*(uint16_t*)(g_base + a)); }
inline uint32_t Read32(uint32_t a) { return _byteswap_ulong(*(uint32_t*)(g_base + a)); }
inline uint64_t Read64(uint32_t a) { return _byteswap_uint64(*(uint64_t*)(g_base + a)); }
inline void Write8(uint32_t a, uint8_t v) { g_base[a] = v; }
inline void Write16(uint32_t a, uint16_t v) { *(uint16_t*)(g_base + a) = _byteswap_ushort(v); }
inline void Write32(uint32_t a, uint32_t v) { *(uint32_t*)(g_base + a) = _byteswap_ulong(v); }
inline void Write64(uint32_t a, uint64_t v) { *(uint64_t*)(g_base + a) = _byteswap_uint64(v); }

// Translate any guest effective address (cached/uncached/physical) to the canonical
// cached virtual address (0x8xxxxxxx / 0x9xxxxxxx).
inline uint32_t CanonAddr(uint32_t a)
{
    uint32_t phys = a & 0x3FFFFFFF;
    if (phys < MEM1_SIZE) return 0x80000000 | phys;
    if (phys >= 0x10000000 && phys < 0x10000000 + MEM2_SIZE) return 0x80000000 | phys;
    return a;
}
// Physical addresses (as given to DMA engines) to host pointers.
inline uint8_t* PhysPtr(uint32_t phys) { return g_base + (0x80000000 | (phys & 0x1FFFFFFF)); }
