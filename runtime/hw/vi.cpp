// Video Interface: beam timing and display interrupts (DI0-DI3).
#include "hw.h"
#include "../cpu/cpu.h"
#include "../kernel/scheduler.h"
#include "../log.h"

static uint16_t s_regs[0x80];   // 0x100 bytes of 16-bit registers
static uint64_t s_t0;           // timebase at half-line 0 of the current timing config
static int s_evt = -1;
uint32_t g_viFieldCount = 0;

static uint16_t& R(uint32_t off) { return s_regs[(off & 0xFF) / 2]; }

static uint32_t EQU() { return R(0x00) & 0xF; }
static uint32_t ACV() { return (R(0x00) >> 4) & 0x3FF; }
static uint32_t PRB_ODD() { return R(0x0E) & 0x3FF; }
static uint32_t PSB_ODD() { return R(0x0C) & 0x3FF; }
static uint32_t PRB_EVEN() { return R(0x12) & 0x3FF; }
static uint32_t PSB_EVEN() { return R(0x10) & 0x3FF; }

static uint32_t HalfLinesOdd() { return 3 * EQU() + PRB_ODD() + 2 * ACV() + PSB_ODD(); }
static uint32_t HalfLinesEven() { return 3 * EQU() + PRB_EVEN() + 2 * ACV() + PSB_EVEN(); }

static uint32_t TotalHalfLines()
{
    uint32_t t = HalfLinesOdd() + HalfLinesEven();
    return t ? t : 1050;
}

static double TicksPerHalfLine()
{
    uint32_t total = TotalHalfLines();
    double frameRate = (total >= 1200) ? 25.0 : 30000.0 / 1001.0;
    return (double)TB_FREQ / (frameRate * total);
}

static uint32_t CurrentHalfLine(uint64_t now)
{
    double hl = (double)(now - s_t0) / TicksPerHalfLine();
    return (uint32_t)((uint64_t)hl % TotalHalfLines());
}

static void UpdateInterrupts()
{
    bool any = false;
    for (int i = 0; i < 4; i++)
    {
        uint16_t hi = R(0x30 + 4 * i);
        if ((hi & 0x8000) && (hi & 0x1000))
            any = true;
    }
    PI_SetInterrupt(INT_CAUSE_VI, any);
}

static uint32_t TargetHalfLine(int i)
{
    uint16_t hi = R(0x30 + 4 * i);
    uint16_t lo = R(0x32 + 4 * i);
    uint32_t vct = hi & 0x7FF;
    uint32_t hct = lo & 0x7FF;
    uint32_t hlw = R(0x06) & 0x1FF;
    if (hlw == 0) hlw = 429;
    if (vct == 0) return ~0u;
    return (vct - 1) * 2 + (hct > hlw ? 1 : 0);
}

static void ScheduleNext()
{
    if (s_evt < 0) return;
    Sched_Cancel(s_evt);
    uint64_t now = Timebase_Now();
    double tphl = TicksPerHalfLine();
    uint32_t total = TotalHalfLines();
    uint64_t absHl = (uint64_t)((double)(now - s_t0) / tphl);
    uint64_t best = ~0ull;
    for (int i = 0; i < 4; i++)
    {
        if (!(R(0x30 + 4 * i) & 0x1000)) continue;   // masked
        uint32_t t = TargetHalfLine(i);
        if (t >= total) continue;
        uint64_t frameStart = absHl - (absHl % total);
        uint64_t cand = frameStart + t;
        if (cand <= absHl) cand += total;
        if (cand < best) best = cand;
    }
    // also tick once per field so field counters advance even without DIs
    uint64_t nextField = absHl + (total / 2) - (absHl % (total / 2));
    if (nextField < best) best = nextField;
    Sched_ScheduleAt(s_evt, s_t0 + (uint64_t)((double)best * tphl) + 1, best);
}

static void OnEvent(uint64_t absHl, int64_t)
{
    uint32_t total = TotalHalfLines();
    uint32_t hl = (uint32_t)(absHl % total);
    if (hl == 0 || hl == total / 2)
        g_viFieldCount++;
    for (int i = 0; i < 4; i++)
    {
        if (TargetHalfLine(i) == hl)
            R(0x30 + 4 * i) |= 0x8000;
    }
    UpdateInterrupts();
    ScheduleNext();
}

uint16_t VI_Read16(uint32_t off)
{
    off &= 0xFF;
    switch (off)
    {
    case 0x2C:   // vertical beam position
        return (uint16_t)(1 + CurrentHalfLine(Timebase_Now()) / 2);
    case 0x2E:   // horizontal beam position
    {
        double tphl = TicksPerHalfLine();
        double hl = (double)(Timebase_Now() - s_t0) / tphl;
        double frac = hl - (double)(uint64_t)hl;
        uint32_t hlw = R(0x06) & 0x1FF;
        if (!hlw) hlw = 429;
        uint32_t odd = ((uint64_t)hl) & 1;
        return (uint16_t)(1 + (odd + frac) * hlw);
    }
    case 0x6C:   // VI clock select (27 MHz)
        return R(off);
    case 0x6E:   // VI DTV status: component cable not connected
        return R(off) | 0;
    default:
        return R(off);
    }
}

void VI_Write16(uint32_t off, uint16_t v)
{
    off &= 0xFF;
    switch (off)
    {
    case 0x30: case 0x34: case 0x38: case 0x3C:
        R(off) = v;
        UpdateInterrupts();
        ScheduleNext();
        break;
    case 0x32: case 0x36: case 0x3A: case 0x3E:
        R(off) = v;
        ScheduleNext();
        break;
    case 0x00: case 0x0C: case 0x0E: case 0x10: case 0x12: case 0x06:
        R(off) = v;
        s_t0 = Timebase_Now();
        ScheduleNext();
        break;
    case 0x02:
        R(off) = v;
        if (v & 2)   // reset
        {
            for (int i = 0; i < 4; i++) R(0x30 + 4 * i) = 0;
            UpdateInterrupts();
        }
        ScheduleNext();
        break;
    default:
        R(off) = v;
        break;
    }
}

uint32_t VI_GetXFBAddrTop() { return ((uint32_t)(R(0x1C) & 0x1FFF) << 16 | R(0x1E)) & 0x00FFFFFF; }

void VI_Init()
{
    // State left behind by the system menu (Dolphin's VideoInterface::Preset).
    R(0x00) = 6;                                   // EQU=6, ACV=0
    R(0x02) = 0x0001 | (1 << 8);                   // ENB, FMT=PAL
    R(0x04) = (uint16_t)(105 | (71 << 8));         // HCE, HCS
    R(0x06) = 429;                                 // HLW
    {
        uint32_t htr1 = 64 | (162u << 7) | (373u << 17);
        R(0x08) = (uint16_t)(htr1 >> 16);
        R(0x0A) = (uint16_t)htr1;
    }
    R(0x0C) = 5;   R(0x0E) = 502;                  // odd field PSB/PRB
    R(0x10) = 4;   R(0x12) = 503;                  // even field PSB/PRB
    R(0x30) = 0x1000 | 263; R(0x32) = 430;         // DI0: retrace, enabled
    R(0x34) = 0x1000 | 1;   R(0x36) = 1;           // DI1: enabled
    R(0x6C) = 0;                                   // 27 MHz clock (PAL)
    R(0x6E) = 1;                                   // component cable plugged (progressive capable)
    s_t0 = Timebase_Now();
    s_evt = Sched_RegisterEvent("VI", OnEvent);
    ScheduleNext();
}
