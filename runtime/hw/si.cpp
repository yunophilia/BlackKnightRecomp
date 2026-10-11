// Serial Interface (GameCube controller ports): no devices attached.
#include "hw.h"
#include "../log.h"

static uint32_t s_regs[0x40];

uint32_t SI_Read32(uint32_t off)
{
    off &= 0xFF;
    switch (off)
    {
    case 0x38:   // SISR: report "no response" on all channels
        return s_regs[off / 4] | 0x08080808;
    default:
        return s_regs[off / 4];
    }
}

void SI_Write32(uint32_t off, uint32_t v)
{
    off &= 0xFF;
    switch (off)
    {
    case 0x34:   // SICOMCSR
        if (v & 1)   // TSTART: complete immediately with "no response"
        {
            v &= ~1u;
            v |= 0x80000000;   // TCINT
            s_regs[0x38 / 4] |= 0x08080808;
        }
        if (v & 0x80000000) { /* ack */ }
        s_regs[off / 4] = v & ~0x80000000u;
        PI_SetInterrupt(INT_CAUSE_SI, false);
        break;
    case 0x38:
        s_regs[off / 4] &= ~v;
        break;
    default:
        s_regs[off / 4] = v;
        break;
    }
}

void SI_Init() {}

// DI registers are owned by IOS on Wii; the PPC should never touch them.
uint32_t DI_Read32(uint32_t off) { LOG_WARN("DI read {:02x}", off); return 0; }
void DI_Write32(uint32_t off, uint32_t v) { LOG_WARN("DI write {:02x} = {:08x}", off, v); }
