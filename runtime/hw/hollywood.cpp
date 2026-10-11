// Hollywood (Wii-specific) registers at 0xCD000000: IPC, IRQ routing, GPIO, misc.
#include "hw.h"
#include "../cpu/cpu.h"
#include "../ios/ios.h"
#include "../kernel/scheduler.h"
#include "../log.h"

enum : uint32_t
{
    INT_CAUSE_IPC_BROADWAY = 0x40000000,
    INT_CAUSE_IPC_STARLET = 0x80000000,
};

struct Ctrl
{
    uint8_t X1 = 0, X2 = 0, Y1 = 0, Y2 = 0, IX1 = 0, IX2 = 0, IY1 = 0, IY2 = 0;
    uint32_t ppc() const { return (IY2 << 5) | (IY1 << 4) | (X2 << 3) | (Y1 << 2) | (Y2 << 1) | X1; }
    void ppc(uint32_t v)
    {
        X1 = v & 1;
        X2 = (v >> 3) & 1;
        if ((v >> 2) & 1) Y1 = 0;
        if ((v >> 1) & 1) Y2 = 0;
        IY1 = (v >> 4) & 1;
        IY2 = (v >> 5) & 1;
    }
};

static Ctrl s_ctrl;
static uint32_t s_ppcMsg = 0, s_armMsg = 0;
static uint32_t s_ppcIrqFlags = 0, s_ppcIrqMask = 0;
static uint32_t s_armIrqFlags = 0, s_armIrqMask = 0;
static uint32_t s_gpioOut = 0, s_gpioDir = 0;
static uint32_t s_resets = 0xFFFFFFFF;
static int s_evtUpdate = -1;

static void UpdateInterrupts()
{
    if ((s_ctrl.Y1 & s_ctrl.IY1) || (s_ctrl.Y2 & s_ctrl.IY2))
        s_ppcIrqFlags |= INT_CAUSE_IPC_BROADWAY;
    if ((s_ctrl.X1 & s_ctrl.IX1) || (s_ctrl.X2 & s_ctrl.IX2))
        s_ppcIrqFlags |= INT_CAUSE_IPC_STARLET;
    PI_SetInterrupt(INT_CAUSE_WII_IPC, (s_ppcIrqFlags & s_ppcIrqMask) != 0);
}

static void OnUpdate(uint64_t, int64_t) { UpdateInterrupts(); }

bool IPC_IsReady()
{
    return s_ctrl.Y1 == 0 && s_ctrl.Y2 == 0 && (s_ppcIrqFlags & INT_CAUSE_IPC_BROADWAY) == 0;
}

void IPC_ClearX1() { s_ctrl.X1 = 0; }

void IPC_GenerateAck()
{
    s_ctrl.Y2 = 1;
    Sched_ScheduleIn(s_evtUpdate, 100);
}

void IPC_GenerateReply(uint32_t addr)
{
    s_armMsg = addr;
    s_ctrl.Y1 = 1;
    Sched_ScheduleIn(s_evtUpdate, 100);
}

uint32_t HLW_Read32(uint32_t off)
{
    switch (off)
    {
    case 0x00: return s_ppcMsg;
    case 0x04: return s_ctrl.ppc();
    case 0x08: return s_armMsg;
    case 0x10: return (uint32_t)(Timebase_Now() * 243 / 128 / 60);   // Starlet timer (~1.9 MHz)
    case 0x30: return s_ppcIrqFlags;
    case 0x34: return s_ppcIrqMask;
    case 0x38: return s_armIrqFlags;
    case 0x3C: return s_armIrqMask;
    case 0x64: return 0xFFFFFFFF;   // AHBPROT
    case 0xC0: return s_gpioOut;
    case 0xC4: return s_gpioDir;
    case 0xC8: return 0x80;         // GPIOB_IN: SLOT_IN (disc present)
    case 0xE0: return s_gpioOut;
    case 0xE4: return s_gpioDir;
    case 0xE8: return 0x80;
    case 0x180: case 0x1CC: case 0x1D0: return 0;
    case 0x194: return s_resets;
    case 0x214: return 0x00000000;  // HW_VERSION-ish
    default:
        LOG_WARN("HLW read {:03x}", off);
        return 0;
    }
}

void HLW_Write32(uint32_t off, uint32_t v)
{
    switch (off)
    {
    case 0x00: s_ppcMsg = v; break;
    case 0x04:
        s_ctrl.ppc(v);
        if (((v >> 2) & 1 && s_ctrl.IY1) || ((v >> 1) & 1 && s_ctrl.IY2))
            s_ppcIrqFlags |= INT_CAUSE_IPC_BROADWAY;
        if (s_ctrl.X1)
            IOS_EnqueueRequest(s_ppcMsg);
        IOS_UpdateIPC();
        UpdateInterrupts();
        break;
    case 0x30:
        s_ppcIrqFlags &= ~v;
        IOS_UpdateIPC();
        UpdateInterrupts();
        break;
    case 0x34:
        s_ppcIrqMask = v;
        if (s_ppcIrqMask & INT_CAUSE_IPC_BROADWAY)
        {
            // writing the Broadway IPC mask resets the IPC state (as in Dolphin)
            s_ctrl = Ctrl();
            s_ppcMsg = s_armMsg = 0;
            s_ppcIrqFlags = 0;
            s_ppcIrqMask = v;
        }
        IOS_UpdateIPC();
        UpdateInterrupts();
        break;
    case 0x38: s_armIrqFlags &= ~v; break;
    case 0x3C: s_armIrqMask = v; break;
    case 0xC0: case 0xE0: s_gpioOut = v; break;
    case 0xC4: case 0xE4: s_gpioDir = v; break;
    case 0x194: s_resets = v; break;
    case 0x18: case 0x24: case 0x180: case 0x1CC: case 0x1D0: break;
    default:
        LOG_WARN("HLW write {:03x} = {:08x}", off, v);
        break;
    }
}

void IPC_Init()
{
    s_evtUpdate = Sched_RegisterEvent("IPCUpdate", OnUpdate);
    s_ppcIrqMask |= INT_CAUSE_IPC_BROADWAY;
}
