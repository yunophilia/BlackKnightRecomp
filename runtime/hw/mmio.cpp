// Memory-mapped I/O dispatch for 0xC8000000-0xCDFFFFFF.
#include <atomic>
#include "hw.h"
#include "../cpu/ppc_context.h"
#include "../kernel/threads.h"
#include "../log.h"

enum class Block { CP, PE, VI, PI, MI, DSP, DI, SI, EXI, AI, GXFIFO, HLW, HLW_DI, EFB, UNKNOWN };

static Block Classify(uint32_t ea, uint32_t& off)
{
    if (ea < 0xCC000000)
    {
        off = ea;
        return Block::EFB;
    }
    if ((ea >> 24) == 0xCD)
    {
        uint32_t o = ea & 0xFFFFFF;
        if (o >= 0x6000 && o < 0x7000)
        {
            // Wii mirrors of DI/SI/EXI/AI
            off = o & 0x3FF;
            switch ((o >> 10) & 3)
            {
            case 0: return Block::HLW_DI;
            case 1: return Block::SI;
            case 2: return Block::EXI;
            default: return Block::AI;
            }
        }
        if (o >= 0x8000 && o < 0x9000)
        {
            off = o - 0x8000;
            return Block::GXFIFO;
        }
        off = o & 0x3FF;
        return Block::HLW;
    }
    uint32_t o = ea & 0xFFFFFF;
    switch (o >> 12)
    {
    case 0x0: off = o & 0xFFF; return Block::CP;
    case 0x1: off = o & 0xFFF; return Block::PE;
    case 0x2: off = o & 0xFFF; return Block::VI;
    case 0x3: off = o & 0xFFF; return Block::PI;
    case 0x4: off = o & 0xFFF; return Block::MI;
    case 0x5: off = o & 0xFFF; return Block::DSP;
    case 0x6:
        off = o & 0x3FF;
        switch ((o >> 10) & 3)
        {
        case 0: return Block::DI;
        case 1: return Block::SI;
        case 2: return Block::EXI;
        default: return Block::AI;
        }
    case 0x8: off = o & 0xFFF; return Block::GXFIFO;
    default: off = o; return Block::UNKNOWN;
    }
}

static uint32_t Read16Block(Block b, uint32_t off)
{
    switch (b)
    {
    case Block::CP: return CP_Read16(off);
    case Block::PE: return PE_Read16(off);
    case Block::VI: return VI_Read16(off);
    case Block::MI: return MI_Read16(off);
    case Block::DSP: return DSP_Read16(off);
    default: return 0;
    }
}

static void Write16Block(Block b, uint32_t off, uint16_t v)
{
    switch (b)
    {
    case Block::CP: CP_Write16(off, v); break;
    case Block::PE: PE_Write16(off, v); break;
    case Block::VI: VI_Write16(off, v); break;
    case Block::MI: MI_Write16(off, v); break;
    case Block::DSP: DSP_Write16(off, v); break;
    default: break;
    }
}

static bool Is16(Block b)
{
    return b == Block::CP || b == Block::PE || b == Block::VI || b == Block::MI || b == Block::DSP;
}

static uint32_t Read32Block(Block b, uint32_t off, uint32_t ea)
{
    switch (b)
    {
    case Block::PI: return PI_Read32(off);
    case Block::SI: return SI_Read32(off);
    case Block::EXI: return EXI_Read32(off);
    case Block::AI: return AI_Read32(off);
    case Block::DI: return DI_Read32(off);
    case Block::HLW: return HLW_Read32(off);
    case Block::HLW_DI: return DI_Read32(off);
    case Block::EFB: { auto lk = GX_SyncGPU(); return EFB_Read32(ea); }
    case Block::DSP: return DSP_Read32(off);
    default:
        LOG_WARN("MMIO read32 from unmapped {:08x}", ea);
        return 0;
    }
}

static void Write32Block(Block b, uint32_t off, uint32_t v, uint32_t ea)
{
    switch (b)
    {
    case Block::PI: PI_Write32(off, v); break;
    case Block::SI: SI_Write32(off, v); break;
    case Block::EXI: EXI_Write32(off, v); break;
    case Block::AI: AI_Write32(off, v); break;
    case Block::DI: DI_Write32(off, v); break;
    case Block::HLW: HLW_Write32(off, v); break;
    case Block::HLW_DI: DI_Write32(off, v); break;
    case Block::EFB: { auto lk = GX_SyncGPU(); EFB_Write32(ea, v); break; }
    case Block::DSP: DSP_Write32(off, v); break;
    case Block::GXFIFO: GPFifo_Write32(v); break;
    default:
        LOG_WARN("MMIO write32 to unmapped {:08x} = {:08x}", ea, v);
        break;
    }
}

extern "C" uint32_t mmio_read32(uint32_t ea)
{
    uint32_t off;
    Block b = Classify(ea, off);
    if (Is16(b))
        return (Read16Block(b, off) << 16) | Read16Block(b, off + 2);
    return Read32Block(b, off, ea);
}

extern "C" uint32_t mmio_read16(uint32_t ea)
{
    uint32_t off;
    Block b = Classify(ea, off);
    if (Is16(b))
        return Read16Block(b, off);
    uint32_t v = Read32Block(b, off & ~3u, ea & ~3u);
    return (off & 2) ? (v & 0xFFFF) : (v >> 16);
}

extern "C" uint32_t mmio_read8(uint32_t ea)
{
    uint32_t v = mmio_read16(ea & ~1u);
    return (ea & 1) ? (v & 0xFF) : (v >> 8);
}

extern "C" void mmio_write32(uint32_t ea, uint32_t v)
{
    uint32_t off;
    Block b = Classify(ea, off);
    if (Is16(b))
    {
        Write16Block(b, off, (uint16_t)(v >> 16));
        Write16Block(b, off + 2, (uint16_t)v);
        return;
    }
    Write32Block(b, off, v, ea);
}

extern "C" void mmio_write16(uint32_t ea, uint32_t v)
{
    uint32_t off;
    Block b = Classify(ea, off);
    if (b == Block::GXFIFO)
    {
        GPFifo_Write16((uint16_t)v);
        return;
    }
    if (Is16(b))
    {
        Write16Block(b, off, (uint16_t)v);
        return;
    }
    // 16-bit write into a 32-bit register: read-modify-write
    uint32_t cur = Read32Block(b, off & ~3u, ea & ~3u);
    if (off & 2) cur = (cur & 0xFFFF0000) | (v & 0xFFFF);
    else cur = (cur & 0xFFFF) | (v << 16);
    Write32Block(b, off & ~3u, cur, ea & ~3u);
}

extern "C" void mmio_write8(uint32_t ea, uint32_t v)
{
    uint32_t off;
    Block b = Classify(ea, off);
    if (b == Block::GXFIFO)
    {
        GPFifo_Write8((uint8_t)v);
        return;
    }
    LOG_WARN("MMIO write8 {:08x} = {:02x}", ea, v);
    uint32_t cur = mmio_read16(ea & ~1u);
    if (ea & 1) cur = (cur & 0xFF00) | (v & 0xFF);
    else cur = (cur & 0xFF) | ((v & 0xFF) << 8);
    mmio_write16(ea & ~1u, cur);
}

// ------------------------------------------------------------------ PI
static std::atomic<uint32_t> s_piCause{ INT_CAUSE_RST_BUTTON };   // also set by the GX thread
static uint32_t s_piMask = 0;
static uint32_t s_piFifoBase, s_piFifoEnd, s_piFifoWptr;
static uint32_t s_piResetCode = 0;

bool HW_PIInterruptPending()
{
    return (s_piCause & s_piMask & 0xFFFF) != 0;
}

void PI_SetInterrupt(uint32_t cause, bool set)
{
    if (set) s_piCause.fetch_or(cause);
    else s_piCause.fetch_and(~cause);
    Irq_Update();
}

uint32_t PI_GetFifoWptr() { return s_piFifoWptr; }
void PI_SetFifoWptr(uint32_t v) { s_piFifoWptr = v; }
uint32_t PI_GetFifoBase() { return s_piFifoBase; }
uint32_t PI_GetFifoEnd() { return s_piFifoEnd; }

uint32_t PI_Read32(uint32_t off)
{
    switch (off)
    {
    case 0x00: return s_piCause;
    case 0x04: return s_piMask;
    case 0x0C: return s_piFifoBase;
    case 0x10: return s_piFifoEnd;
    case 0x14: return s_piFifoWptr;
    case 0x24: return s_piResetCode;
    case 0x2C: return 0x246500B1;   // Flipper/Hollywood revision
    default:
        LOG_WARN("PI read {:02x}", off);
        return 0;
    }
}

void PI_Write32(uint32_t off, uint32_t v)
{
    switch (off)
    {
    case 0x00: s_piCause.fetch_and(~(v & 0xFFFF & ~INT_CAUSE_WII_IPC)); Irq_Update(); break;
    case 0x04: s_piMask = v; Irq_Update(); break;
    case 0x0C: s_piFifoBase = v & 0x1FFFFFE0; break;
    case 0x10: s_piFifoEnd = v & 0x1FFFFFE0; break;
    case 0x14: s_piFifoWptr = v & 0x1FFFFFE0; break;
    case 0x18: break;   // FIFO reset
    case 0x24: s_piResetCode = v; LOG_INFO("PI reset code {:08x}", v); break;
    default:
        LOG_WARN("PI write {:02x} = {:08x}", off, v);
        break;
    }
}

// ------------------------------------------------------------------ MI (memory protection; stub)
static uint16_t s_mi[0x80];
uint16_t MI_Read16(uint32_t off) { return off < 0x100 ? s_mi[off / 2] : 0; }
void MI_Write16(uint32_t off, uint16_t v) { if (off < 0x100) s_mi[off / 2] = v; }

void HW_Init()
{
    VI_Init();
    EXI_Init();
    DSP_Init();
    SI_Init();
    IPC_Init();
    GX_Init();
}
