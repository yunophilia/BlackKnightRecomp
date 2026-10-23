// CPU-level runtime support for recompiled code: indirect calls, SPRs, timebase,
// decrementer, MSR and assorted rarely used instructions.
#include "ppc_context.h"
#include "cpu.h"
#include "../log.h"
#include "../kernel/memory.h"
#include "../kernel/threads.h"
#include "../region.h"

#include <atomic>
#include <bit>
#include <limits>
#include <unordered_map>
#include <vector>
#include <windows.h>

static constexpr uint32_t CODE_BEGIN = 0x80004000;
static constexpr uint32_t CODE_END = 0x80700000;
static std::vector<PPCFunc> s_table;

// The HOME menu module, recompiled at its boot-time link address. Its functions are
// used only once the code in guest memory is verified to be that exact image.
static std::vector<PPCFunc> s_rsoTable;
static uint32_t s_rsoBegin = 0, s_rsoEnd = 0;   // empty unless the release's module is recompiled
static int s_rsoState = 0;   // 0 unchecked, 1 verified, <0 mismatch (re-check countdown)

static bool RsoVerified()
{
    if (s_rsoState > 0) return true;
    if (s_rsoState < 0 && ++s_rsoState < 0) return false;
    const uint8_t* p = g_base + s_rsoBegin;
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < s_rsoEnd - s_rsoBegin; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    if (h == Region_Code().rsoTextHash)
    {
        s_rsoState = 1;
        LOG_INFO("HOME menu module matches the recompiled image");
        return true;
    }
    static bool warned = false;
    if (!warned) { warned = true; LOG_INFO("HOME menu module differs from the recompiled image; interpreting it"); }
    s_rsoState = -100000;
    return false;
}

extern "C" volatile uint32_t g_ppcIrqPending = 0;
extern "C" uint32_t g_cfgFrameRate = 30;
extern "C" void Interp_Run(PPCContext& ctx, uint8_t* base, uint32_t entry);

void Cpu_Init()
{
    const PPCRegionCode& code = Region_Code();
    s_table.assign((CODE_END - CODE_BEGIN) / 4, nullptr);
    size_t n = 0;
    s_rsoBegin = code.rsoTextBegin;
    s_rsoEnd = code.rsoTextEnd;
    s_rsoTable.assign((s_rsoEnd - s_rsoBegin) / 4, nullptr);
    for (const PPCFuncMapping* m = code.funcs; m->host; m++, n++)
    {
        if (m->guest >= s_rsoBegin && m->guest < s_rsoEnd)
        {
            s_rsoTable[(m->guest - s_rsoBegin) / 4] = m->host;
            continue;
        }
        s_table[(m->guest - CODE_BEGIN) / 4] = m->host;
    }
    LOG_INFO("registered {} recompiled functions", n);
    Timebase_Init();
}

void Cpu_SetFunction(uint32_t guest, PPCFunc f)
{
    s_table[(guest - CODE_BEGIN) / 4] = f;
}

extern "C" PPCFunc ppc_lookup(uint32_t addr)
{
    if (addr >= s_rsoBegin && addr < s_rsoEnd && !(addr & 3))
        return RsoVerified() ? s_rsoTable[(addr - s_rsoBegin) / 4] : nullptr;
    if (addr < CODE_BEGIN || addr >= CODE_END || (addr & 3))
        return nullptr;
    return s_table[(addr - CODE_BEGIN) / 4];
}

extern "C" void ppc_call_indirect(PPCContext& ctx, uint8_t* base, uint32_t addr)
{
    PPCFunc f = ppc_lookup(addr);
    if (!f) [[unlikely]]
    {
        uint32_t phys = addr & 0x3FFFFFFF;
        bool inRam = (addr >= 0x80000000) && (phys < MEM1_SIZE || (phys >= 0x10000000 && phys < 0x10000000 + MEM2_SIZE));
        if (!inRam || (addr & 3))
            FATAL("indirect call to invalid address {:08x} (lr={:08x})", addr, ctx.lr);
        // BKR_DUMPMEM2=1: dump MEM2 on the first call into it (input for tools/rso.py)
        static const bool dumpMem2 = getenv("BKR_DUMPMEM2") != nullptr;
        if (dumpMem2)
        {
            static bool dumped = false;
            if (!dumped && addr >= 0x90000000)
            {
                dumped = true;
                if (FILE* f = fopen("mem2_dump.bin", "wb")) { fwrite(base + 0x90000000, 1, 0x100000, f); fclose(f); }
            }
        }
        // Module import trampoline (stw r12,4(r1); lis/addi r12,target; mtctr r12;
        // lwz r12,4(r1); bctr): jump straight to the (recompiled) target.
        const uint8_t* p = base + addr;
        auto w = [&](int i) { return _byteswap_ulong(*(const uint32_t*)(p + 4 * i)); };
        if (w(0) == 0x91810004 && (w(1) >> 16) == 0x3D80 && (w(2) >> 16) == 0x398C && w(3) == 0x7D8903A6 && w(4) == 0x81810004 &&
            w(5) == 0x4E800420)
        {
            uint32_t target = (w(1) << 16) + (uint32_t)(int32_t)(int16_t)(w(2) & 0xFFFF);
            Write32(ctx.r[1] + 4, ctx.r[12]);
            ctx.ctr = target;
            if (ppc_lookup(target))
            {
                ppc_call_indirect(ctx, base, target);
                return;
            }
        }
        static std::unordered_map<uint32_t, int> seen;
        if (seen[addr]++ == 0) LOG_INFO("interpreting non-recompiled code at {:08x} (lr={:08x})", addr, ctx.lr);
        Interp_Run(ctx, base, addr);
        return;
    }
    f(ctx, base);
}

// ---------------------------------------------------------------- timebase
// Wii timebase ticks at bus clock / 4 = 243 MHz / 4.
static LARGE_INTEGER s_qpcFreq, s_qpcStart;
static uint64_t s_tbOffset = 0;

void Timebase_Init()
{
    QueryPerformanceFrequency(&s_qpcFreq);
    QueryPerformanceCounter(&s_qpcStart);
}

uint64_t Timebase_Now()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t d = (uint64_t)(now.QuadPart - s_qpcStart.QuadPart);
    uint64_t hi = d / (uint64_t)s_qpcFreq.QuadPart;
    uint64_t lo = d % (uint64_t)s_qpcFreq.QuadPart;
    return s_tbOffset + hi * TB_FREQ + lo * TB_FREQ / (uint64_t)s_qpcFreq.QuadPart;
}

// Pause (window focus loss): the guest thread parks here; on resume the time base
// continues from where it stopped, so game time and scheduled events do not jump.
std::atomic<bool> g_pauseRequested{ false };
void Cpu_PauseIfRequested()
{
    if (!g_pauseRequested.load(std::memory_order_relaxed)) return;
    uint64_t tb = Timebase_Now();
    LOG_INFO("paused");
    while (g_pauseRequested.load()) Sleep(20);
    Timebase_Set(tb);
    LOG_INFO("resumed");
}

void Timebase_Set(uint64_t v)
{
    s_tbOffset = 0;
    s_tbOffset = v - Timebase_Now();
}

extern "C" uint32_t ppc_mftb(PPCContext& ctx, uint32_t tbr)
{
    uint64_t tb = Timebase_Now();
    return tbr == 269 ? (uint32_t)(tb >> 32) : (uint32_t)tb;
}

// ---------------------------------------------------------------- decrementer
static uint64_t s_decDeadline = 0;
static bool s_decArmed = false;

uint32_t Dec_Read()
{
    int64_t remaining = (int64_t)(s_decDeadline - Timebase_Now());
    return (uint32_t)remaining;
}

void Dec_Write(uint32_t v)
{
    s_decDeadline = Timebase_Now() + (int64_t)(int32_t)v;
    s_decArmed = !(v & 0x80000000);
    Sched_Kick();
}

bool Dec_Expired()
{
    return s_decArmed && (int64_t)(Timebase_Now() - s_decDeadline) >= 0;
}

void Dec_Ack()
{
    s_decArmed = false;
}

uint64_t Dec_Deadline(bool* armed)
{
    *armed = s_decArmed;
    return s_decDeadline;
}

// ---------------------------------------------------------------- SPRs
extern "C" uint32_t ppc_mfspr(PPCContext& ctx, uint32_t spr)
{
    switch (spr)
    {
    case 18: return ctx.dsisr;
    case 19: return ctx.dar;
    case 22: return Dec_Read();
    case 26: return ctx.srr0;
    case 27: return ctx.srr1;
    case 268: return ppc_mftb(ctx, 268);
    case 269: return ppc_mftb(ctx, 269);
    case 272: case 273: case 274: case 275: return ctx.sprg[spr - 272];
    case 282: return ctx.ear;
    case 287: return 0x00087102;   // PVR: Broadway
    case 920: return ctx.hid2;
    case 921: return ctx.wpar;
    case 922: return ctx.dma_u;
    case 923: return ctx.dma_l;
    case 936: return ctx.mmcr0;  // UMMCR0
    case 952: return ctx.mmcr0;
    case 956: return ctx.mmcr1;
    case 953: case 954: case 957: case 958: return 0;   // PMC
    case 1008: return ctx.hid0;
    case 1009: return ctx.hid1;
    case 1011: return ctx.hid4;
    case 1017: return ctx.l2cr;
    case 1019: return ctx.ictc;
    case 1020: case 1021: case 1022: return ctx.thrm[spr - 1020];
    default:
        LOG_WARN("mfspr unhandled spr {} (lr={:08x})", spr, ctx.lr);
        return 0;
    }
}

extern "C" void ppc_mtspr(PPCContext& ctx, uint8_t* base, uint32_t spr, uint32_t v)
{
    switch (spr)
    {
    case 18: ctx.dsisr = v; break;
    case 19: ctx.dar = v; break;
    case 22: Dec_Write(v); break;
    case 26: ctx.srr0 = v; break;
    case 27: ctx.srr1 = v; break;
    case 284: Timebase_Set((Timebase_Now() & 0xFFFFFFFF00000000ull) | v); break;
    case 285: Timebase_Set((Timebase_Now() & 0xFFFFFFFFull) | ((uint64_t)v << 32)); break;
    case 272: case 273: case 274: case 275: ctx.sprg[spr - 272] = v; break;
    case 282: ctx.ear = v; break;
    case 920: ctx.hid2 = v; break;
    case 921: ctx.wpar = v; break;
    case 922: ctx.dma_u = v; break;
    case 923:
        ctx.dma_l = v;
        if (v & 2)   // DMA trigger (locked cache)
        {
            uint32_t memAddr = ctx.dma_u & ~31u;
            uint32_t lcAddr = v & ~31u;
            uint32_t len = ((ctx.dma_u & 0x1F) << 2) | ((v >> 2) & 3);
            if (len == 0) len = 128;
            len *= 32;
            if (v & 0x10)   // load: memory -> locked cache
                memcpy(base + lcAddr, base + memAddr, len);
            else
                memcpy(base + memAddr, base + lcAddr, len);
            ctx.dma_l &= ~2u;
        }
        break;
    case 952: case 936: ctx.mmcr0 = v; break;
    case 956: ctx.mmcr1 = v; break;
    case 953: case 954: case 957: case 958: break;
    case 1008: ctx.hid0 = v; break;
    case 1009: ctx.hid1 = v; break;
    case 1011: ctx.hid4 = v; break;
    case 1017: ctx.l2cr = v; break;
    case 1019: ctx.ictc = v; break;
    case 1020: case 1021: case 1022: ctx.thrm[spr - 1020] = v; break;
    default:
        if (spr >= 528 && spr <= 575) break;   // BAT registers: no MMU emulation
        LOG_WARN("mtspr unhandled spr {} = {:08x} (lr={:08x})", spr, v, ctx.lr);
        break;
    }
}

extern bool g_irqDeferred;   // threads.cpp: interrupt arrived while MSR[EE] was off
extern "C" void ppc_mtmsr(PPCContext& ctx, uint8_t* base, uint32_t v)
{
    uint32_t old = ctx.msr;
    ctx.msr = v;
    if ((v & MSR_EE) && !(old & MSR_EE) && (g_ppcIrqPending || g_irqDeferred))
    {
        g_irqDeferred = false;
        ppc_check_irq(ctx, base);
    }
}

extern "C" void ppc_sc(PPCContext& ctx, uint8_t* base, uint32_t addr)
{
    // The SDK's system call only synchronises caches (PPCSync); nothing to do.
}

// rfi: used by the SDK's RealMode() helper to jump to a function with translation
// off, and by that function to return (srr0 == lr). Treat as call / return.
extern "C" void ppc_rfi(PPCContext& ctx, uint8_t* base)
{
    ctx.msr = ctx.srr1;
    uint32_t t = ctx.srr0;
    if (t < 0x80000000) t |= 0x80000000;
    if (t == ctx.lr)
        return;   // generated code returns right after this call
    PPCFunc f = ppc_lookup(t);
    if (!f)
        FATAL("rfi to unknown address {:08x} (lr={:08x})", ctx.srr0, ctx.lr);
    f(ctx, base);
}

extern "C" void ppc_trap(PPCContext& ctx, uint8_t* base, uint32_t addr, uint32_t to, uint32_t a, uint32_t b)
{
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    bool t = ((to & 16) && sa < sb) || ((to & 8) && sa > sb) || ((to & 4) && sa == sb) ||
             ((to & 2) && a < b) || ((to & 1) && a > b);
    if (t)
        LOG_ERROR("trap at {:08x} (lr={:08x})", addr, ctx.lr);
}

extern "C" void ppc_illegal(PPCContext& ctx, uint8_t* base, uint32_t addr, uint32_t insn)
{
    FATAL("illegal instruction {:08x} at {:08x}", insn, addr);
}

extern "C" void ppc_lswi(PPCContext& ctx, uint8_t* base, uint32_t ea, uint32_t rd, uint32_t nb)
{
    if (nb == 0) nb = 32;
    uint32_t r = rd - 1, i = 0;
    while (nb > 0)
    {
        if (i == 0)
        {
            r = (r + 1) & 31;
            ctx.r[r] = 0;
        }
        ctx.r[r] |= ld8(base, ea) << (24 - i);
        i = (i + 8) & 31;
        ea++;
        nb--;
    }
}

extern "C" void ppc_stswi(PPCContext& ctx, uint8_t* base, uint32_t ea, uint32_t rs, uint32_t nb)
{
    if (nb == 0) nb = 32;
    uint32_t r = rs - 1, i = 0;
    while (nb > 0)
    {
        if (i == 0) r = (r + 1) & 31;
        st8(base, ea, (ctx.r[r] >> (24 - i)) & 0xFF);
        i = (i + 8) & 31;
        ea++;
        nb--;
    }
}

extern "C" void ppc_lswx(PPCContext& ctx, uint8_t* base, uint32_t ea, uint32_t rd)
{
    uint32_t n = ctx.xer_bc;
    for (uint32_t k = 0; k < n; k++)
    {
        uint32_t reg = (rd + k / 4) & 31;
        if ((k & 3) == 0) ctx.r[reg] = 0;
        ctx.r[reg] |= ld8(base, ea + k) << (24 - 8 * (k & 3));
    }
}

extern "C" void ppc_stswx(PPCContext& ctx, uint8_t* base, uint32_t ea, uint32_t rs)
{
    uint32_t n = ctx.xer_bc;
    for (uint32_t k = 0; k < n; k++)
    {
        uint32_t reg = (rs + k / 4) & 31;
        st8(base, ea + k, (ctx.r[reg] >> (24 - 8 * (k & 3))) & 0xFF);
    }
}

// ---------------------------------------------------------------- estimates (exact Gekko tables, from Dolphin)
struct BaseAndDec { int m_base; int m_dec; };
static const BaseAndDec frsqrte_expected[32] = {
    {0x1a7e800, -0x568}, {0x17cb800, -0x4f3}, {0x1552800, -0x48d}, {0x130c000, -0x435},
    {0x10f2000, -0x3e7}, {0x0eff000, -0x3a2}, {0x0d2e000, -0x365}, {0x0b7c000, -0x32e},
    {0x09e5000, -0x2fc}, {0x0867000, -0x2d0}, {0x06ff000, -0x2a8}, {0x05ab800, -0x283},
    {0x046a000, -0x261}, {0x0339800, -0x243}, {0x0218800, -0x226}, {0x0105800, -0x20b},
    {0x3ffa000, -0x7a4}, {0x3c29000, -0x700}, {0x38aa000, -0x670}, {0x3572000, -0x5f2},
    {0x3279000, -0x584}, {0x2fb7000, -0x524}, {0x2d26000, -0x4cc}, {0x2ac0000, -0x47e},
    {0x2881000, -0x43a}, {0x2665000, -0x3fa}, {0x2468000, -0x3c2}, {0x2287000, -0x38e},
    {0x20c1000, -0x35e}, {0x1f12000, -0x332}, {0x1d79000, -0x30a}, {0x1bf4000, -0x2e6},
};
static const BaseAndDec fres_expected[32] = {
    {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340}, {0x638800, 0x313},
    {0x5d6200, 0x2ea}, {0x579000, 0x2c4}, {0x520800, 0x2a0}, {0x4cc800, 0x27f}, {0x47ca00, 0x261},
    {0x430800, 0x245}, {0x3e8000, 0x22a}, {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5},
    {0x2e4a00, 0x1d1}, {0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b},
    {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b}, {0x124400, 0x143},
    {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d}, {0x088400, 0x11a}, {0x065000, 0x11a},
    {0x041c00, 0x108}, {0x020c00, 0x106},
};

static double MakeQuiet(double d)
{
    uint64_t i = std::bit_cast<uint64_t>(d) | (1ull << 51);
    return std::bit_cast<double>(i);
}

extern "C" double ppc_frsqrte(double val)
{
    int64_t integral = std::bit_cast<int64_t>(val);
    int64_t mantissa = integral & ((1LL << 52) - 1);
    const int64_t sign = integral & (1ULL << 63);
    int64_t exponent = integral & (0x7FFLL << 52);
    if (mantissa == 0 && exponent == 0)
        return sign ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    if (exponent == (0x7FFLL << 52))
    {
        if (mantissa == 0)
            return sign ? std::numeric_limits<double>::quiet_NaN() : 0.0;
        return MakeQuiet(val);
    }
    if (sign)
        return std::numeric_limits<double>::quiet_NaN();
    if (!exponent)
    {
        do
        {
            exponent -= 1LL << 52;
            mantissa <<= 1;
        } while (!(mantissa & (1LL << 52)));
        mantissa &= (1LL << 52) - 1;
        exponent += 1LL << 52;
    }
    const int64_t exponent_lsb = exponent & (1LL << 52);
    exponent = ((0x3FFLL << 52) - ((exponent - (0x3FELL << 52)) / 2)) & (0x7FFLL << 52);
    integral = sign | exponent;
    const int i = (int)((exponent_lsb | mantissa) >> 37);
    const auto& entry = frsqrte_expected[i / 2048];
    integral |= (int64_t)(entry.m_base + entry.m_dec * (i % 2048)) << 26;
    return std::bit_cast<double>(integral);
}

extern "C" double ppc_fres(double val)
{
    int64_t integral = std::bit_cast<int64_t>(val);
    const int64_t mantissa = integral & ((1LL << 52) - 1);
    const int64_t sign = integral & (1ULL << 63);
    int64_t exponent = integral & (0x7FFLL << 52);
    if (mantissa == 0 && exponent == 0)
        return std::copysign(std::numeric_limits<double>::infinity(), val);
    if (exponent == (0x7FFLL << 52))
    {
        if (mantissa == 0)
            return std::copysign(0.0, val);
        return MakeQuiet(val);
    }
    if (exponent < (895LL << 52))
        return std::copysign((double)std::numeric_limits<float>::max(), val);
    if (exponent >= (1149LL << 52))
        return std::copysign(0.0, val);
    exponent = (0x7FDLL << 52) - exponent;
    const int i = (int)(mantissa >> 37);
    const auto& entry = fres_expected[i / 1024];
    integral = sign | exponent;
    integral |= (int64_t)(entry.m_base - (entry.m_dec * (i % 1024) + 1) / 2) << 29;
    return std::bit_cast<double>(integral);
}
