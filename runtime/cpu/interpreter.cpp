// Fallback PowerPC interpreter for code that was not statically recompiled
// (dynamically loaded modules such as hbm_part.rso, or stray indirect jumps).
// Semantics mirror tools/recomp.py. Calls into addresses that have recompiled
// functions run natively.
#include "ppc_context.h"
#include "cpu.h"
#include "../log.h"

#include <cmath>

static constexpr uint32_t RETURN_MAGIC = 0xFFFFFFF0u;

static inline int32_t SExt16(uint32_t v) { return (int16_t)(v & 0xFFFF); }
static inline uint32_t Mask(uint32_t mb, uint32_t me)
{
    uint32_t a = 0xFFFFFFFFu >> mb, b = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? (a & b) : (a | b);
}

static inline bool CondOK(PPCContext& ctx, uint32_t bo, uint32_t bi)
{
    bool ctrOk = true;
    if (!(bo & 4))
    {
        ctx.ctr--;
        ctrOk = (bo & 2) ? ctx.ctr == 0 : ctx.ctr != 0;
    }
    bool condOk = (bo & 0x10) || (ctx.cr[bi] == ((bo >> 3) & 1));
    return ctrOk && condOk;
}

static inline void CR0(PPCContext& ctx, uint32_t v) { PPC_CR0(v); }

// Branch to a target as a call: native if available, otherwise keep interpreting.
// Returns true if handled natively (pc should continue at nxt).
static inline bool NativeCall(PPCContext& ctx, uint8_t* base, uint32_t target)
{
    PPCFunc f = ppc_lookup(target);
    if (!f) return false;
    f(ctx, base);
    return true;
}

volatile uint32_t g_interpPC = 0;   // sampled by the profiler

extern "C" void Interp_Run(PPCContext& ctx, uint8_t* base, uint32_t entry)
{
    uint32_t savedLr = ctx.lr;
    ctx.lr = RETURN_MAGIC;
    uint32_t pc = entry;
    uint64_t steps = 0;
    for (;;)
    {
        if (pc == RETURN_MAGIC) break;
        g_interpPC = pc;
        uint32_t w = ld32n(base, pc);
        uint32_t op = w >> 26;
        uint32_t rd = (w >> 21) & 31, ra = (w >> 16) & 31, rb = (w >> 11) & 31, rc_ = (w >> 6) & 31;
        bool rc = w & 1;
        uint32_t nxt = pc + 4;
        uint32_t ea;
        auto RA0 = [&]() -> uint32_t { return ra ? ctx.r[ra] : 0; };
        if ((++steps & 0xFFF) == 0) PPC_CHECK_IRQ();
        switch (op)
        {
        case 3: ppc_trap(ctx, base, pc, rd, ctx.r[ra], (uint32_t)SExt16(w)); break;
        case 7: ctx.r[rd] = (uint32_t)((int32_t)ctx.r[ra] * SExt16(w)); break;
        case 8: { uint64_t t = (uint64_t)(uint32_t)~ctx.r[ra] + (uint32_t)SExt16(w) + 1; ctx.r[rd] = (uint32_t)t; ctx.xer_ca = (uint8_t)(t >> 32); break; }
        case 10: PPC_CMPU((w >> 23) & 7, ctx.r[ra], w & 0xFFFF); break;
        case 11: PPC_CMPS((w >> 23) & 7, (int32_t)ctx.r[ra], SExt16(w)); break;
        case 12: case 13: { uint64_t t = (uint64_t)ctx.r[ra] + (uint32_t)SExt16(w); ctx.r[rd] = (uint32_t)t; ctx.xer_ca = (uint8_t)(t >> 32); if (op == 13) CR0(ctx, ctx.r[rd]); break; }
        case 14: ctx.r[rd] = RA0() + SExt16(w); break;
        case 15: ctx.r[rd] = RA0() + (w << 16); break;
        case 16:
        {
            uint32_t bo = rd, bi = ra;
            int32_t bd = (int16_t)(w & 0xFFFC);
            uint32_t t = (w & 2) ? (uint32_t)bd : pc + bd;
            if (CondOK(ctx, bo, bi))
            {
                if (w & 1)
                {
                    ctx.lr = nxt;
                    if (NativeCall(ctx, base, t)) break;
                }
                if (t <= pc) PPC_CHECK_IRQ();
                pc = t;
                continue;
            }
            break;
        }
        case 17: ppc_sc(ctx, base, pc); break;
        case 18:
        {
            int32_t li = (int32_t)((w & 0x03FFFFFC) << 6) >> 6;
            uint32_t t = (w & 2) ? (uint32_t)li : pc + li;
            if (w & 1)
            {
                ctx.lr = nxt;
                if (t == nxt) break;
                if (NativeCall(ctx, base, t)) break;
                pc = t;
                continue;
            }
            // tail branch: if it targets a native function, run it and return
            if (PPCFunc f = ppc_lookup(t))
            {
                f(ctx, base);
                pc = ctx.lr;
                continue;
            }
            if (t <= pc) PPC_CHECK_IRQ();
            pc = t;
            continue;
        }
        case 19:
        {
            uint32_t xo = (w >> 1) & 0x3FF;
            switch (xo)
            {
            case 0: memcpy(&ctx.cr[4 * ((w >> 23) & 7)], &ctx.cr[4 * ((w >> 18) & 7)], 4); break;
            case 16:   // bclr
            {
                uint32_t t = ctx.lr;
                if (CondOK(ctx, rd, ra))
                {
                    if (w & 1)
                    {
                        ctx.lr = nxt;
                        if (NativeCall(ctx, base, t)) break;
                    }
                    pc = t;
                    continue;
                }
                break;
            }
            case 528:   // bcctr
            {
                uint32_t t = ctx.ctr;
                if (CondOK(ctx, rd | 4, ra))
                {
                    if (w & 1)
                    {
                        ctx.lr = nxt;
                        if (NativeCall(ctx, base, t)) break;
                        pc = t;
                        continue;
                    }
                    if (PPCFunc f = ppc_lookup(t))
                    {
                        f(ctx, base);
                        pc = ctx.lr;
                        continue;
                    }
                    pc = t;
                    continue;
                }
                break;
            }
            case 33: ctx.cr[rd] = !(ctx.cr[ra] | ctx.cr[rb]); break;
            case 129: ctx.cr[rd] = ctx.cr[ra] & !ctx.cr[rb]; break;
            case 193: ctx.cr[rd] = ctx.cr[ra] ^ ctx.cr[rb]; break;
            case 225: ctx.cr[rd] = !(ctx.cr[ra] & ctx.cr[rb]); break;
            case 257: ctx.cr[rd] = ctx.cr[ra] & ctx.cr[rb]; break;
            case 289: ctx.cr[rd] = !(ctx.cr[ra] ^ ctx.cr[rb]); break;
            case 417: ctx.cr[rd] = ctx.cr[ra] | !ctx.cr[rb]; break;
            case 449: ctx.cr[rd] = ctx.cr[ra] | ctx.cr[rb]; break;
            case 150: break;   // isync
            case 50: ppc_rfi(ctx, base); pc = ctx.lr; continue;
            default: FATAL("interp: unhandled op19 xo {} at {:08x}", xo, pc);
            }
            break;
        }
        case 20: case 21: case 23:
        {
            uint32_t sh = op == 23 ? (ctx.r[rb] & 31) : rb;
            uint32_t m = Mask((w >> 6) & 31, (w >> 1) & 31);
            uint32_t r = _rotl(ctx.r[rd], sh);
            ctx.r[ra] = op == 20 ? ((r & m) | (ctx.r[ra] & ~m)) : (r & m);
            if (rc) CR0(ctx, ctx.r[ra]);
            break;
        }
        case 24: ctx.r[ra] = ctx.r[rd] | (w & 0xFFFF); break;
        case 25: ctx.r[ra] = ctx.r[rd] | (w << 16); break;
        case 26: ctx.r[ra] = ctx.r[rd] ^ (w & 0xFFFF); break;
        case 27: ctx.r[ra] = ctx.r[rd] ^ (w << 16); break;
        case 28: ctx.r[ra] = ctx.r[rd] & (w & 0xFFFF); CR0(ctx, ctx.r[ra]); break;
        case 29: ctx.r[ra] = ctx.r[rd] & (w << 16); CR0(ctx, ctx.r[ra]); break;
        case 31:
        {
            uint32_t xo = (w >> 1) & 0x3FF;
            uint32_t xo9 = xo & 0x1FF;
            uint32_t A = ctx.r[ra], B = ctx.r[rb], S = ctx.r[rd];
            bool done = true;
            switch (xo9)
            {
            case 8: { uint64_t t = (uint64_t)(uint32_t)~A + B + 1; ctx.r[rd] = (uint32_t)t; ctx.xer_ca = (uint8_t)(t >> 32); break; }
            case 10: { uint64_t t = (uint64_t)A + B; ctx.r[rd] = (uint32_t)t; ctx.xer_ca = (uint8_t)(t >> 32); break; }
            case 11: ctx.r[rd] = (uint32_t)(((uint64_t)A * B) >> 32); break;
            case 40: ctx.r[rd] = B - A; break;
            case 75: ctx.r[rd] = (uint32_t)(((int64_t)(int32_t)A * (int32_t)B) >> 32); break;
            case 104: ctx.r[rd] = (uint32_t)-(int32_t)A; break;
            case 136: { uint64_t t = (uint64_t)(uint32_t)~A + B + ctx.xer_ca; ctx.r[rd] = (uint32_t)t; ctx.xer_ca = (uint8_t)(t >> 32); break; }
            case 138: { uint64_t t = (uint64_t)A + B + ctx.xer_ca; ctx.r[rd] = (uint32_t)t; ctx.xer_ca = (uint8_t)(t >> 32); break; }
            case 200: { uint64_t t = (uint64_t)(uint32_t)~A + ctx.xer_ca; ctx.r[rd] = (uint32_t)t; ctx.xer_ca = (uint8_t)(t >> 32); break; }
            case 202: { uint64_t t = (uint64_t)A + ctx.xer_ca; ctx.r[rd] = (uint32_t)t; ctx.xer_ca = (uint8_t)(t >> 32); break; }
            case 232: { uint64_t t = (uint64_t)(uint32_t)~A + ctx.xer_ca + 0xFFFFFFFFull; ctx.r[rd] = (uint32_t)t; ctx.xer_ca = (uint8_t)(t >> 32); break; }
            case 234: { uint64_t t = (uint64_t)A + ctx.xer_ca + 0xFFFFFFFFull; ctx.r[rd] = (uint32_t)t; ctx.xer_ca = (uint8_t)(t >> 32); break; }
            case 235: ctx.r[rd] = (uint32_t)((int32_t)A * (int32_t)B); break;
            case 266: ctx.r[rd] = A + B; break;
            case 459: ctx.r[rd] = ppc_divwu(A, B); break;
            case 491: ctx.r[rd] = ppc_divw(A, B); break;
            default: done = false; break;
            }
            if (done)
            {
                if (rc) CR0(ctx, ctx.r[rd]);
                break;
            }
            // X-form loads/stores
            auto X = [&]() { return (ra ? A : 0) + B; };
            switch (xo)
            {
            case 0: PPC_CMPS((w >> 23) & 7, (int32_t)A, (int32_t)B); break;
            case 32: PPC_CMPU((w >> 23) & 7, A, B); break;
            case 4: ppc_trap(ctx, base, pc, rd, A, B); break;
            case 19: ctx.r[rd] = ppc_get_cr(ctx); break;
            case 144: ppc_mtcrf(ctx, S, (w >> 12) & 0xFF); break;
            case 83: ctx.r[rd] = ctx.msr; break;
            case 146: ppc_mtmsr(ctx, base, S); break;
            case 339:
            {
                uint32_t spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5);
                if (spr == 8) ctx.r[rd] = ctx.lr;
                else if (spr == 9) ctx.r[rd] = ctx.ctr;
                else if (spr == 1) ctx.r[rd] = ppc_get_xer(ctx);
                else if (spr >= 912 && spr < 920) ctx.r[rd] = ctx.gqr[spr - 912];
                else ctx.r[rd] = ppc_mfspr(ctx, spr);
                break;
            }
            case 467:
            {
                uint32_t spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5);
                if (spr == 8) ctx.lr = S;
                else if (spr == 9) ctx.ctr = S;
                else if (spr == 1) ppc_set_xer(ctx, S);
                else if (spr >= 912 && spr < 920) ctx.gqr[spr - 912] = S;
                else ppc_mtspr(ctx, base, spr, S);
                break;
            }
            case 371: ctx.r[rd] = ppc_mftb(ctx, ((w >> 16) & 31) | (((w >> 11) & 31) << 5)); break;
            case 24: ctx.r[ra] = (B & 0x20) ? 0 : (S << (B & 31)); if (rc) CR0(ctx, ctx.r[ra]); break;
            case 536: ctx.r[ra] = (B & 0x20) ? 0 : (S >> (B & 31)); if (rc) CR0(ctx, ctx.r[ra]); break;
            case 792: ctx.r[ra] = ppc_sraw(ctx, S, B); if (rc) CR0(ctx, ctx.r[ra]); break;
            case 824:
            {
                uint32_t n = rb;
                ctx.xer_ca = n && ((int32_t)S < 0) && (S & ((1u << n) - 1));
                ctx.r[ra] = (uint32_t)((int32_t)S >> n);
                if (rc) CR0(ctx, ctx.r[ra]);
                break;
            }
            case 26: ctx.r[ra] = ppc_cntlzw(S); if (rc) CR0(ctx, ctx.r[ra]); break;
            case 28: ctx.r[ra] = S & B; if (rc) CR0(ctx, ctx.r[ra]); break;
            case 60: ctx.r[ra] = S & ~B; if (rc) CR0(ctx, ctx.r[ra]); break;
            case 124: ctx.r[ra] = ~(S | B); if (rc) CR0(ctx, ctx.r[ra]); break;
            case 284: ctx.r[ra] = ~(S ^ B); if (rc) CR0(ctx, ctx.r[ra]); break;
            case 316: ctx.r[ra] = S ^ B; if (rc) CR0(ctx, ctx.r[ra]); break;
            case 412: ctx.r[ra] = S | ~B; if (rc) CR0(ctx, ctx.r[ra]); break;
            case 444: ctx.r[ra] = S | B; if (rc) CR0(ctx, ctx.r[ra]); break;
            case 476: ctx.r[ra] = ~(S & B); if (rc) CR0(ctx, ctx.r[ra]); break;
            case 922: ctx.r[ra] = (uint32_t)(int32_t)(int16_t)S; if (rc) CR0(ctx, ctx.r[ra]); break;
            case 954: ctx.r[ra] = (uint32_t)(int32_t)(int8_t)S; if (rc) CR0(ctx, ctx.r[ra]); break;
            case 23: ctx.r[rd] = ld32(base, X()); break;
            case 55: ea = A + B; ctx.r[rd] = ld32(base, ea); ctx.r[ra] = ea; break;
            case 87: ctx.r[rd] = ld8(base, X()); break;
            case 119: ea = A + B; ctx.r[rd] = ld8(base, ea); ctx.r[ra] = ea; break;
            case 279: ctx.r[rd] = ld16(base, X()); break;
            case 311: ea = A + B; ctx.r[rd] = ld16(base, ea); ctx.r[ra] = ea; break;
            case 343: ctx.r[rd] = (uint32_t)(int32_t)(int16_t)ld16(base, X()); break;
            case 375: ea = A + B; ctx.r[rd] = (uint32_t)(int32_t)(int16_t)ld16(base, ea); ctx.r[ra] = ea; break;
            case 151: st32(base, X(), S); break;
            case 183: ea = A + B; st32(base, ea, S); ctx.r[ra] = ea; break;
            case 215: st8(base, X(), S); break;
            case 247: ea = A + B; st8(base, ea, S); ctx.r[ra] = ea; break;
            case 407: st16(base, X(), S); break;
            case 439: ea = A + B; st16(base, ea, S); ctx.r[ra] = ea; break;
            case 534: ctx.r[rd] = _byteswap_ulong(ld32(base, X())); break;
            case 790: ctx.r[rd] = _byteswap_ushort((uint16_t)ld16(base, X())); break;
            case 662: st32(base, X(), _byteswap_ulong(S)); break;
            case 918: st16(base, X(), _byteswap_ushort((uint16_t)S)); break;
            case 20: ea = X(); ctx.reserve = 1; ctx.reserve_addr = ea; ctx.r[rd] = ld32(base, ea); break;
            case 150: st32(base, X(), S); ctx.cr[0] = 0; ctx.cr[1] = 0; ctx.cr[2] = 1; ctx.cr[3] = ctx.xer_so; break;
            case 535: { double v = ldf32(base, X()); ctx.f[rd].f64 = v; ctx.f[rd].ps1 = v; break; }
            case 567: { ea = A + B; double v = ldf32(base, ea); ctx.f[rd].f64 = v; ctx.f[rd].ps1 = v; ctx.r[ra] = ea; break; }
            case 599: ctx.f[rd].u64 = ld64(base, X()); break;
            case 631: ea = A + B; ctx.f[rd].u64 = ld64(base, ea); ctx.r[ra] = ea; break;
            case 663: stf32(base, X(), (float)ctx.f[rd].f64); break;
            case 695: ea = A + B; stf32(base, ea, (float)ctx.f[rd].f64); ctx.r[ra] = ea; break;
            case 727: st64(base, X(), ctx.f[rd].u64); break;
            case 759: ea = A + B; st64(base, ea, ctx.f[rd].u64); ctx.r[ra] = ea; break;
            case 983: st32(base, X(), (uint32_t)ctx.f[rd].u64); break;
            case 1014: ppc_dcbz(base, X()); break;
            case 597: ppc_lswi(ctx, base, RA0(), rd, rb); break;
            case 725: ppc_stswi(ctx, base, RA0(), rd, rb); break;
            case 54: case 86: case 246: case 278: case 470: case 982: case 598: case 854: case 306: case 566: case 370: break;
            case 595: ctx.r[rd] = ctx.sr[(w >> 16) & 15]; break;
            case 210: ctx.sr[(w >> 16) & 15] = S; break;
            default: FATAL("interp: unhandled op31 xo {} at {:08x}", xo, pc);
            }
            break;
        }
        case 32: ctx.r[rd] = ld32(base, RA0() + SExt16(w)); break;
        case 33: ea = ctx.r[ra] + SExt16(w); ctx.r[rd] = ld32(base, ea); ctx.r[ra] = ea; break;
        case 34: ctx.r[rd] = ld8(base, RA0() + SExt16(w)); break;
        case 35: ea = ctx.r[ra] + SExt16(w); ctx.r[rd] = ld8(base, ea); ctx.r[ra] = ea; break;
        case 36: st32(base, RA0() + SExt16(w), ctx.r[rd]); break;
        case 37: ea = ctx.r[ra] + SExt16(w); st32(base, ea, ctx.r[rd]); ctx.r[ra] = ea; break;
        case 38: st8(base, RA0() + SExt16(w), ctx.r[rd]); break;
        case 39: ea = ctx.r[ra] + SExt16(w); st8(base, ea, ctx.r[rd]); ctx.r[ra] = ea; break;
        case 40: ctx.r[rd] = ld16(base, RA0() + SExt16(w)); break;
        case 41: ea = ctx.r[ra] + SExt16(w); ctx.r[rd] = ld16(base, ea); ctx.r[ra] = ea; break;
        case 42: ctx.r[rd] = (uint32_t)(int32_t)(int16_t)ld16(base, RA0() + SExt16(w)); break;
        case 43: ea = ctx.r[ra] + SExt16(w); ctx.r[rd] = (uint32_t)(int32_t)(int16_t)ld16(base, ea); ctx.r[ra] = ea; break;
        case 44: st16(base, RA0() + SExt16(w), ctx.r[rd]); break;
        case 45: ea = ctx.r[ra] + SExt16(w); st16(base, ea, ctx.r[rd]); ctx.r[ra] = ea; break;
        case 46: ea = RA0() + SExt16(w); for (uint32_t r = rd; r < 32; r++, ea += 4) ctx.r[r] = ld32(base, ea); break;
        case 47: ea = RA0() + SExt16(w); for (uint32_t r = rd; r < 32; r++, ea += 4) st32(base, ea, ctx.r[r]); break;
        case 48: { double v = ldf32(base, RA0() + SExt16(w)); ctx.f[rd].f64 = v; ctx.f[rd].ps1 = v; break; }
        case 49: { ea = ctx.r[ra] + SExt16(w); double v = ldf32(base, ea); ctx.f[rd].f64 = v; ctx.f[rd].ps1 = v; ctx.r[ra] = ea; break; }
        case 50: ctx.f[rd].u64 = ld64(base, RA0() + SExt16(w)); break;
        case 51: ea = ctx.r[ra] + SExt16(w); ctx.f[rd].u64 = ld64(base, ea); ctx.r[ra] = ea; break;
        case 52: stf32(base, RA0() + SExt16(w), (float)ctx.f[rd].f64); break;
        case 53: ea = ctx.r[ra] + SExt16(w); stf32(base, ea, (float)ctx.f[rd].f64); ctx.r[ra] = ea; break;
        case 54: st64(base, RA0() + SExt16(w), ctx.f[rd].u64); break;
        case 55: ea = ctx.r[ra] + SExt16(w); st64(base, ea, ctx.f[rd].u64); ctx.r[ra] = ea; break;
        case 56: case 57: case 60: case 61:
        {
            int32_t d = (int32_t)((w & 0xFFF) << 20) >> 20;
            uint32_t W = (w >> 15) & 1, I = (w >> 12) & 7;
            ea = ((op == 57 || op == 61) ? ctx.r[ra] : RA0()) + d;
            if (op < 60) ppc_psq_load(ctx, base, ea, W, I, rd);
            else ppc_psq_store(ctx, base, ea, W, I, rd);
            if (op == 57 || op == 61) ctx.r[ra] = ea;
            break;
        }
        case 59: case 63:
        {
            double a = ctx.f[ra].f64, b = ctx.f[rb].f64, c = ctx.f[rc_].f64;
            uint32_t xo5 = (w >> 1) & 31, xo10 = (w >> 1) & 0x3FF;
            bool single = op == 59;
            auto setS = [&](double v) { double r = (double)(float)v; ctx.f[rd].f64 = r; ctx.f[rd].ps1 = r; };
            auto setD = [&](double v) { ctx.f[rd].f64 = v; };
            auto set = [&](double v) { if (single) setS(v); else setD(v); };
            if (op == 63 && (xo10 == 0 || xo10 == 32)) { PPC_FCMP((w >> 23) & 7, a, b); break; }
            if (op == 63)
            {
                bool handled = true;
                switch (xo10)
                {
                case 12: setS(b); break;
                case 14: ctx.f[rd].u64 = ppc_fctiw(ctx, b); break;
                case 15: ctx.f[rd].u64 = ppc_fctiwz(b); break;
                case 40: ctx.f[rd].u64 = ctx.f[rb].u64 ^ 0x8000000000000000ull; break;
                case 72: ctx.f[rd].u64 = ctx.f[rb].u64; break;
                case 136: ctx.f[rd].u64 = ctx.f[rb].u64 | 0x8000000000000000ull; break;
                case 264: ctx.f[rd].u64 = ctx.f[rb].u64 & 0x7FFFFFFFFFFFFFFFull; break;
                case 583: ctx.f[rd].u64 = 0xFFF8000000000000ull | ctx.fpscr; break;
                case 711: ppc_mtfsf(ctx, (w >> 17) & 0xFF, (uint32_t)ctx.f[rb].u64); break;
                case 38: ctx.fpscr |= 0x80000000u >> rd; break;
                case 70: ctx.fpscr &= ~(0x80000000u >> rd); break;
                case 134: { uint32_t sh = 28 - 4 * ((w >> 23) & 7); ctx.fpscr = (ctx.fpscr & ~(0xFu << sh)) | (((w >> 12) & 15) << sh); break; }
                default: handled = false; break;
                }
                if (handled) { if (rc) PPC_CR1(); break; }
            }
            switch (xo5)
            {
            case 18: set(a / b); break;
            case 20: set(a - b); break;
            case 21: set(a + b); break;
            case 22: set(std::sqrt(b)); break;
            case 23: setD(a >= 0.0 ? c : b); break;
            case 24: setS(ppc_fres(b)); break;
            case 25: set(a * c); break;
            case 26: setD(ppc_frsqrte(b)); break;
            case 28: set(a * c - b); break;
            case 29: set(a * c + b); break;
            case 30: set(-(a * c - b)); break;
            case 31: set(-(a * c + b)); break;
            default: FATAL("interp: unhandled fp op {} xo {} at {:08x}", op, xo5, pc);
            }
            if (rc) PPC_CR1();
            break;
        }
        case 4:
        {
            uint32_t xo10 = (w >> 1) & 0x3FF, xo6 = (w >> 1) & 0x3F, xo5 = (w >> 1) & 31;
            double a0 = ctx.f[ra].f64, a1 = ctx.f[ra].ps1, b0 = ctx.f[rb].f64, b1 = ctx.f[rb].ps1, c0 = ctx.f[rc_].f64, c1 = ctx.f[rc_].ps1;
            auto set = [&](double x0, double x1) { ctx.f[rd].f64 = (double)(float)x0; ctx.f[rd].ps1 = (double)(float)x1; };
            auto setRaw = [&](double x0, double x1) { ctx.f[rd].f64 = x0; ctx.f[rd].ps1 = x1; };
            switch (xo10)
            {
            case 0: case 32: PPC_FCMP((w >> 23) & 7, a0, b0); goto ps_done;
            case 64: case 96: PPC_FCMP((w >> 23) & 7, a1, b1); goto ps_done;
            case 40: setRaw(-b0, -b1); goto ps_done;
            case 72: setRaw(b0, b1); goto ps_done;
            case 136: setRaw(-std::fabs(b0), -std::fabs(b1)); goto ps_done;
            case 264: setRaw(std::fabs(b0), std::fabs(b1)); goto ps_done;
            case 528: setRaw(a0, b0); goto ps_done;
            case 560: setRaw(a0, b1); goto ps_done;
            case 592: setRaw(a1, b0); goto ps_done;
            case 624: setRaw(a1, b1); goto ps_done;
            case 1014: ppc_dcbz(base, RA0() + ctx.r[rb]); goto ps_done;
            }
            if (xo6 == 6 || xo6 == 38 || xo6 == 7 || xo6 == 39)
            {
                uint32_t W = (w >> 10) & 1, I = (w >> 7) & 7;
                bool upd = xo6 >= 38;
                ea = (upd || ra) ? ctx.r[ra] + ctx.r[rb] : ctx.r[rb];
                if (xo6 == 6 || xo6 == 38) ppc_psq_load(ctx, base, ea, W, I, rd);
                else ppc_psq_store(ctx, base, ea, W, I, rd);
                if (upd) ctx.r[ra] = ea;
                goto ps_done;
            }
            switch (xo5)
            {
            case 10: set(a0 + b1, c1); break;
            case 11: set(c0, a0 + b1); break;
            case 12: set(a0 * c0, a1 * c0); break;
            case 13: set(a0 * c1, a1 * c1); break;
            case 14: set(a0 * c0 + b0, a1 * c0 + b1); break;
            case 15: set(a0 * c1 + b0, a1 * c1 + b1); break;
            case 18: set(a0 / b0, a1 / b1); break;
            case 20: set(a0 - b0, a1 - b1); break;
            case 21: set(a0 + b0, a1 + b1); break;
            case 23: setRaw(a0 >= 0.0 ? c0 : b0, a1 >= 0.0 ? c1 : b1); break;
            case 24: setRaw(ppc_fres(b0), ppc_fres(b1)); break;
            case 25: set(a0 * c0, a1 * c1); break;
            case 26: setRaw((double)(float)ppc_frsqrte(b0), (double)(float)ppc_frsqrte(b1)); break;
            case 28: set(a0 * c0 - b0, a1 * c1 - b1); break;
            case 29: set(a0 * c0 + b0, a1 * c1 + b1); break;
            case 30: set(-(a0 * c0 - b0), -(a1 * c1 - b1)); break;
            case 31: set(-(a0 * c0 + b0), -(a1 * c1 + b1)); break;
            default: FATAL("interp: unhandled paired op xo {} at {:08x}", xo5, pc);
            }
        ps_done:
            break;
        }
        default:
            FATAL("interp: illegal/unhandled instruction {:08x} at {:08x}", w, pc);
        }
        pc = nxt;
    }
    ctx.lr = savedLr;
}
