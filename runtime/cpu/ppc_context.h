#pragma once
// Guest CPU state and the primitives used by recompiled code.

#include <cstdint>
#include <cstring>
#include <bit>
#include <cmath>
#include <climits>
#include <stdlib.h>

// The recompiled code builds with MSVC, clang-cl, clang and GCC; the runtime itself
// is Windows-only for now.
#ifdef _MSC_VER
#include <intrin.h>
#define PPC_FORCEINLINE __forceinline
#define ppc_bswap16 _byteswap_ushort
#define ppc_bswap32 _byteswap_ulong
#define ppc_bswap64 _byteswap_uint64
#define ppc_rotl32 _rotl
#else
#define PPC_FORCEINLINE inline __attribute__((always_inline))
#define ppc_bswap16 __builtin_bswap16
#define ppc_bswap32 __builtin_bswap32
#define ppc_bswap64 __builtin_bswap64
inline uint32_t ppc_rotl32(uint32_t v, int n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
#endif

union PPCFPR
{
    struct
    {
        union
        {
            double f64;
            uint64_t u64;
        };
        double ps1;
    };
};

struct PPCContext
{
    uint32_t r[32];
    PPCFPR f[32];
    uint32_t lr;
    uint32_t ctr;
    uint8_t cr[32];        // one byte per CR bit: [4n+0]=LT [4n+1]=GT [4n+2]=EQ [4n+3]=SO
    uint8_t xer_so, xer_ov, xer_ca, xer_bc;
    uint32_t msr;
    uint32_t fpscr;
    uint32_t gqr[8];
    uint32_t sr[16];
    uint32_t srr0, srr1;
    uint32_t sprg[4];
    uint32_t hid0, hid1, hid2, hid4, wpar, dma_u, dma_l, l2cr, dar, dsisr, ear;
    uint32_t mmcr0, mmcr1, pmc[4], thrm[3], ictc;
    uint32_t reserve, reserve_addr;
};

typedef void (*PPCFunc)(PPCContext& __restrict ctx, uint8_t* __restrict base);

struct PPCFuncMapping
{
    uint32_t guest;
    PPCFunc host;
};

// The recompiled code of one release (tools/recomp.py), in that release's namespace
struct PPCRegionCode
{
    const char* gameId;              // RENE8P, RENP8P, RENJ8P
    const PPCFuncMapping* funcs;     // every function, ends with { 0, nullptr }
    const PPCFunc* originals;        // per runtime/game_addrs.inc entry: the recompiled
                                     // original of a function the runtime overrides, or null
    uint32_t addrCount;              // entries in runtime/game_addrs.inc at generation
    uint32_t rsoTextBegin, rsoTextEnd;   // HOME menu module recompiled at its link address
    uint64_t rsoTextHash;                // (tools/rso.py), all 0 if not recompiled
};

#define PPC_EXTERN_FUNC(x) extern "C" void x(PPCContext& __restrict ctx, uint8_t* __restrict base)
#define PPC_FUNC(x) extern "C" void x(PPCContext& __restrict ctx, uint8_t* __restrict base)
// a recompiled function, inside its release's namespace
#define PPC_GEN_FUNC(x) void x(PPCContext& __restrict ctx, uint8_t* __restrict base)
#define PPC_FUNC_PROLOGUE() uint32_t ea; (void)ea
// Optional call counting for frame-rate analysis (BKR_CALL_COUNTS builds only):
// one counter per main.dol function entry, indexed by address.
#ifdef BKR_CALL_COUNTS
extern "C" uint32_t g_callCounts[0x200000];
#define PPC_COUNT_CALL(a) do { if ((a) - 0x80000000u < 0x00800000u) g_callCounts[((a) - 0x80000000u) >> 2]++; } while (0)
#else
#define PPC_COUNT_CALL(a) ((void)0)
#endif
#define PPC_NOP() ((void)0)

// ------------------------------------------------------------------ runtime hooks
extern "C" {
extern volatile uint32_t g_ppcIrqPending;
extern uint32_t g_tasProbe[16];   // read-only test probes (tools/recomp.py PROBES)
PPCFunc ppc_lookup(uint32_t addr);
void ppc_call_indirect(PPCContext& ctx, uint8_t* base, uint32_t addr);
void ppc_check_irq(PPCContext& ctx, uint8_t* base);

uint32_t mmio_read8(uint32_t ea);
uint32_t mmio_read16(uint32_t ea);
uint32_t mmio_read32(uint32_t ea);
void mmio_write8(uint32_t ea, uint32_t v);
void mmio_write16(uint32_t ea, uint32_t v);
void mmio_write32(uint32_t ea, uint32_t v);

uint32_t ppc_mfspr(PPCContext& ctx, uint32_t spr);
void ppc_mtspr(PPCContext& ctx, uint8_t* base, uint32_t spr, uint32_t v);
uint32_t ppc_mftb(PPCContext& ctx, uint32_t tbr);
void ppc_mtmsr(PPCContext& ctx, uint8_t* base, uint32_t v);
void ppc_sc(PPCContext& ctx, uint8_t* base, uint32_t addr);
void ppc_rfi(PPCContext& ctx, uint8_t* base);
void ppc_trap(PPCContext& ctx, uint8_t* base, uint32_t addr, uint32_t to, uint32_t a, uint32_t b);
void ppc_illegal(PPCContext& ctx, uint8_t* base, uint32_t addr, uint32_t insn);
void ppc_lswi(PPCContext& ctx, uint8_t* base, uint32_t ea, uint32_t rd, uint32_t nb);
void ppc_stswi(PPCContext& ctx, uint8_t* base, uint32_t ea, uint32_t rs, uint32_t nb);
void ppc_lswx(PPCContext& ctx, uint8_t* base, uint32_t ea, uint32_t rd);
void ppc_stswx(PPCContext& ctx, uint8_t* base, uint32_t ea, uint32_t rs);
double ppc_fres(double v);
double ppc_frsqrte(double v);
}

#define PPC_CALL_INDIRECT(addr) ppc_call_indirect(ctx, base, (addr))
#define PPC_CHECK_IRQ() do { if (g_ppcIrqPending) ppc_check_irq(ctx, base); } while (0)

// ------------------------------------------------------------------ memory
// Guest addresses map 1:1 into a 4 GiB host reservation at `base`.
// 0xC8000000..0xCDFFFFFF (EFB, hardware registers) is trapped to MMIO handlers.

PPC_FORCEINLINE bool ppc_is_mmio(uint32_t ea) { return (uint32_t)(ea - 0xC8000000u) < 0x06000000u; }

PPC_FORCEINLINE uint32_t ld8n(uint8_t* base, uint32_t ea) { return base[ea]; }
PPC_FORCEINLINE uint32_t ld16n(uint8_t* base, uint32_t ea) { return ppc_bswap16(*(uint16_t*)(base + ea)); }
PPC_FORCEINLINE uint32_t ld32n(uint8_t* base, uint32_t ea) { return ppc_bswap32(*(uint32_t*)(base + ea)); }
PPC_FORCEINLINE void st8n(uint8_t* base, uint32_t ea, uint32_t v) { base[ea] = (uint8_t)v; }
PPC_FORCEINLINE void st16n(uint8_t* base, uint32_t ea, uint32_t v) { *(uint16_t*)(base + ea) = ppc_bswap16((uint16_t)v); }
PPC_FORCEINLINE void st32n(uint8_t* base, uint32_t ea, uint32_t v) { *(uint32_t*)(base + ea) = ppc_bswap32(v); }

PPC_FORCEINLINE uint32_t ld8(uint8_t* base, uint32_t ea)
{
    if (ppc_is_mmio(ea)) [[unlikely]] return mmio_read8(ea);
    return base[ea];
}
PPC_FORCEINLINE uint32_t ld16(uint8_t* base, uint32_t ea)
{
    if (ppc_is_mmio(ea)) [[unlikely]] return mmio_read16(ea);
    return ppc_bswap16(*(uint16_t*)(base + ea));
}
PPC_FORCEINLINE uint32_t ld32(uint8_t* base, uint32_t ea)
{
    if (ppc_is_mmio(ea)) [[unlikely]] return mmio_read32(ea);
    return ppc_bswap32(*(uint32_t*)(base + ea));
}
PPC_FORCEINLINE void st8(uint8_t* base, uint32_t ea, uint32_t v)
{
    if (ppc_is_mmio(ea)) [[unlikely]] { mmio_write8(ea, v); return; }
    base[ea] = (uint8_t)v;
}
PPC_FORCEINLINE void st16(uint8_t* base, uint32_t ea, uint32_t v)
{
    if (ppc_is_mmio(ea)) [[unlikely]] { mmio_write16(ea, v); return; }
    *(uint16_t*)(base + ea) = ppc_bswap16((uint16_t)v);
}
PPC_FORCEINLINE void st32(uint8_t* base, uint32_t ea, uint32_t v)
{
    if (ppc_is_mmio(ea)) [[unlikely]] { mmio_write32(ea, v); return; }
    *(uint32_t*)(base + ea) = ppc_bswap32(v);
}
PPC_FORCEINLINE uint64_t ld64(uint8_t* base, uint32_t ea)
{
    if (ppc_is_mmio(ea)) [[unlikely]] return ((uint64_t)mmio_read32(ea) << 32) | mmio_read32(ea + 4);
    return ppc_bswap64(*(uint64_t*)(base + ea));
}
PPC_FORCEINLINE void st64(uint8_t* base, uint32_t ea, uint64_t v)
{
    if (ppc_is_mmio(ea)) [[unlikely]] { mmio_write32(ea, (uint32_t)(v >> 32)); mmio_write32(ea + 4, (uint32_t)v); return; }
    *(uint64_t*)(base + ea) = ppc_bswap64(v);
}
PPC_FORCEINLINE float ldf32(uint8_t* base, uint32_t ea)
{
    uint32_t u = ld32(base, ea);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
PPC_FORCEINLINE void stf32(uint8_t* base, uint32_t ea, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    st32(base, ea, u);
}

PPC_FORCEINLINE void ppc_dcbz(uint8_t* base, uint32_t ea)
{
    ea &= ~31u;
    if (ppc_is_mmio(ea)) return;
    memset(base + ea, 0, 32);
}
PPC_FORCEINLINE void ppc_dcbi(uint8_t* base, uint32_t ea) { (void)base; (void)ea; }

// ------------------------------------------------------------------ integer helpers
#define PPC_CMPS(f, a, b) do { int32_t a_ = (a), b_ = (b); ctx.cr[4*(f)] = a_ < b_; ctx.cr[4*(f)+1] = a_ > b_; ctx.cr[4*(f)+2] = a_ == b_; ctx.cr[4*(f)+3] = ctx.xer_so; } while (0)
#define PPC_CMPU(f, a, b) do { uint32_t a_ = (a), b_ = (b); ctx.cr[4*(f)] = a_ < b_; ctx.cr[4*(f)+1] = a_ > b_; ctx.cr[4*(f)+2] = a_ == b_; ctx.cr[4*(f)+3] = ctx.xer_so; } while (0)
#define PPC_CR0(v) PPC_CMPS(0, (int32_t)(v), 0)
#define PPC_CR1() do { ctx.cr[4] = (ctx.fpscr >> 31) & 1; ctx.cr[5] = (ctx.fpscr >> 30) & 1; ctx.cr[6] = (ctx.fpscr >> 29) & 1; ctx.cr[7] = (ctx.fpscr >> 28) & 1; } while (0)
#define PPC_FCMP(f, a, b) do { double a_ = (a), b_ = (b); ctx.cr[4*(f)] = a_ < b_; ctx.cr[4*(f)+1] = a_ > b_; ctx.cr[4*(f)+2] = a_ == b_; ctx.cr[4*(f)+3] = (a_ != a_) || (b_ != b_); \
    ctx.fpscr = (ctx.fpscr & ~0xF000u) | ((uint32_t)ctx.cr[4*(f)] << 15) | ((uint32_t)ctx.cr[4*(f)+1] << 14) | ((uint32_t)ctx.cr[4*(f)+2] << 13) | ((uint32_t)ctx.cr[4*(f)+3] << 12); } while (0)

PPC_FORCEINLINE uint32_t ppc_divw(uint32_t a, uint32_t b)
{
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    if (sb == 0 || (sa == INT32_MIN && sb == -1))
        return (sa < 0) ? 0xFFFFFFFFu : 0;
    return (uint32_t)(sa / sb);
}
PPC_FORCEINLINE uint32_t ppc_divwu(uint32_t a, uint32_t b)
{
    return b == 0 ? 0 : a / b;
}
PPC_FORCEINLINE uint32_t ppc_cntlzw(uint32_t v)
{
    return (uint32_t)std::countl_zero(v);
}
PPC_FORCEINLINE uint32_t ppc_sraw(PPCContext& ctx, uint32_t s, uint32_t b)
{
    uint32_t n = b & 0x3F;
    int32_t v = (int32_t)s;
    if (n >= 32)
    {
        ctx.xer_ca = v < 0;
        return (uint32_t)(v >> 31);
    }
    if (n == 0)
    {
        ctx.xer_ca = 0;
        return s;
    }
    ctx.xer_ca = (v < 0) && (s & ((1u << n) - 1));
    return (uint32_t)(v >> n);
}

PPC_FORCEINLINE uint32_t ppc_get_xer(PPCContext& ctx)
{
    return ((uint32_t)ctx.xer_so << 31) | ((uint32_t)ctx.xer_ov << 30) | ((uint32_t)ctx.xer_ca << 29) | ctx.xer_bc;
}
PPC_FORCEINLINE void ppc_set_xer(PPCContext& ctx, uint32_t v)
{
    ctx.xer_so = (v >> 31) & 1;
    ctx.xer_ov = (v >> 30) & 1;
    ctx.xer_ca = (v >> 29) & 1;
    ctx.xer_bc = v & 0x7F;
}
PPC_FORCEINLINE uint32_t ppc_get_cr(PPCContext& ctx)
{
    uint32_t v = 0;
    for (int i = 0; i < 32; i++)
        v |= (uint32_t)(ctx.cr[i] & 1) << (31 - i);
    return v;
}
PPC_FORCEINLINE void ppc_set_cr(PPCContext& ctx, uint32_t v)
{
    for (int i = 0; i < 32; i++)
        ctx.cr[i] = (v >> (31 - i)) & 1;
}
PPC_FORCEINLINE void ppc_mtcrf(PPCContext& ctx, uint32_t v, uint32_t crm)
{
    for (int f = 0; f < 8; f++)
        if (crm & (0x80 >> f))
            for (int b = 0; b < 4; b++)
                ctx.cr[4 * f + b] = (v >> (31 - (4 * f + b))) & 1;
}

// ------------------------------------------------------------------ floating point helpers
PPC_FORCEINLINE uint64_t ppc_fctiwz(double v)
{
    int32_t r;
    if (v != v) r = INT32_MIN;
    else if (v >= 2147483647.0) r = INT32_MAX;
    else if (v <= -2147483648.0) r = INT32_MIN;
    else r = (int32_t)v;
    return 0xFFF8000000000000ull | (uint32_t)r;
}
PPC_FORCEINLINE uint64_t ppc_fctiw(PPCContext& ctx, double v)
{
    double rv;
    switch (ctx.fpscr & 3)
    {
    case 0: rv = nearbyint(v); break;      // assumes host round-to-nearest-even
    case 1: rv = trunc(v); break;
    case 2: rv = ceil(v); break;
    default: rv = floor(v); break;
    }
    return ppc_fctiwz(rv);
}
PPC_FORCEINLINE void ppc_mtfsf(PPCContext& ctx, uint32_t fm, uint32_t v)
{
    uint32_t m = 0;
    for (int i = 0; i < 8; i++)
        if (fm & (1u << i))
            m |= 0xFu << (4 * i);
    ctx.fpscr = (ctx.fpscr & ~m) | (v & m);
}

// ------------------------------------------------------------------ paired singles
PPC_FORCEINLINE float ppc_dequant(uint32_t raw, uint32_t type, int scale_bits, uint32_t /*unused*/ = 0)
{
    static const float kScale[64] = {
#define S(n) (1.0f / (float)(1ull << (n)))
        1.0f, S(1), S(2), S(3), S(4), S(5), S(6), S(7), S(8), S(9), S(10), S(11), S(12), S(13), S(14), S(15),
        S(16), S(17), S(18), S(19), S(20), S(21), S(22), S(23), S(24), S(25), S(26), S(27), S(28), S(29), S(30), S(31),
#undef S
#define L(n) ((float)(1ull << (n)))
        L(32), L(31), L(30), L(29), L(28), L(27), L(26), L(25), L(24), L(23), L(22), L(21), L(20), L(19), L(18), L(17),
        L(16), L(15), L(14), L(13), L(12), L(11), L(10), L(9), L(8), L(7), L(6), L(5), L(4), L(3), L(2), L(1),
#undef L
    };
    float f;
    switch (type)
    {
    case 4: f = (float)(uint8_t)raw; break;
    case 5: f = (float)(uint16_t)raw; break;
    case 6: f = (float)(int8_t)raw; break;
    case 7: f = (float)(int16_t)raw; break;
    default: memcpy(&f, &raw, 4); return f;
    }
    return f * kScale[scale_bits & 63];
}

PPC_FORCEINLINE uint32_t ppc_quant(float v, uint32_t type, int scale_bits)
{
    static const float kScale[64] = {
#define L(n) ((float)(1ull << (n)))
        1.0f, L(1), L(2), L(3), L(4), L(5), L(6), L(7), L(8), L(9), L(10), L(11), L(12), L(13), L(14), L(15),
        L(16), L(17), L(18), L(19), L(20), L(21), L(22), L(23), L(24), L(25), L(26), L(27), L(28), L(29), L(30), L(31),
#undef L
#define S(n) (1.0f / (float)(1ull << (n)))
        S(32), S(31), S(30), S(29), S(28), S(27), S(26), S(25), S(24), S(23), S(22), S(21), S(20), S(19), S(18), S(17),
        S(16), S(15), S(14), S(13), S(12), S(11), S(10), S(9), S(8), S(7), S(6), S(5), S(4), S(3), S(2), S(1),
#undef S
    };
    if (type == 0)
    {
        uint32_t u;
        memcpy(&u, &v, 4);
        return u;
    }
    float s = v * kScale[scale_bits & 63];
    float lo, hi;
    switch (type)
    {
    case 4: lo = 0; hi = 255; break;
    case 5: lo = 0; hi = 65535; break;
    case 6: lo = -128; hi = 127; break;
    case 7: lo = -32768; hi = 32767; break;
    default: lo = 0; hi = 0; break;
    }
    if (!(s > lo)) s = lo;   // also catches NaN
    if (s > hi) s = hi;
    return (uint32_t)(int32_t)s;
}

PPC_FORCEINLINE void ppc_psq_load(PPCContext& ctx, uint8_t* base, uint32_t ea, uint32_t w, uint32_t i, uint32_t d)
{
    uint32_t gqr = ctx.gqr[i];
    uint32_t type = (gqr >> 16) & 7;
    int scale = (gqr >> 24) & 0x3F;
    float p0, p1;
    switch (type)
    {
    case 4: case 6:
        p0 = ppc_dequant(ld8(base, ea), type, scale);
        p1 = w ? 1.0f : ppc_dequant(ld8(base, ea + 1), type, scale);
        break;
    case 5: case 7:
        p0 = ppc_dequant(ld16(base, ea), type, scale);
        p1 = w ? 1.0f : ppc_dequant(ld16(base, ea + 2), type, scale);
        break;
    default:
        p0 = ldf32(base, ea);
        p1 = w ? 1.0f : ldf32(base, ea + 4);
        break;
    }
    ctx.f[d].f64 = (double)p0;
    ctx.f[d].ps1 = (double)p1;
}

PPC_FORCEINLINE void ppc_psq_store(PPCContext& ctx, uint8_t* base, uint32_t ea, uint32_t w, uint32_t i, uint32_t s)
{
    uint32_t gqr = ctx.gqr[i];
    uint32_t type = gqr & 7;
    int scale = (gqr >> 8) & 0x3F;
    float p0 = (float)ctx.f[s].f64;
    float p1 = (float)ctx.f[s].ps1;
    switch (type)
    {
    case 4: case 6:
        st8(base, ea, ppc_quant(p0, type, scale));
        if (!w) st8(base, ea + 1, ppc_quant(p1, type, scale));
        break;
    case 5: case 7:
        st16(base, ea, ppc_quant(p0, type, scale));
        if (!w) st16(base, ea + 2, ppc_quant(p1, type, scale));
        break;
    default:
        stf32(base, ea, p0);
        if (!w) stf32(base, ea + 4, p1);
        break;
    }
}
