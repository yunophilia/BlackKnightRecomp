// High-level replacements for a few SDK functions that only exist for debugging output.
#include "memory.h"
#include "../log.h"
#include "../../generated/ppc_recomp_shared.h"

#include <string>

// Minimal guest printf: pulls integer arguments from r[firstGpr..10] then the
// stack overflow area, floating point arguments from f1..f8.
std::string GuestFormat(PPCContext& ctx, uint32_t fmtAddr, int firstGpr)
{
    std::string out;
    int gpr = firstGpr, fpr = 1;
    uint32_t stackArg = ctx.r[1] + 8;
    auto nextInt = [&]() -> uint32_t {
        if (gpr <= 10) return ctx.r[gpr++];
        uint32_t v = Read32(stackArg);
        stackArg += 4;
        return v;
    };
    auto nextDouble = [&]() -> double {
        if (fpr <= 8) return ctx.f[fpr++].f64;
        stackArg = (stackArg + 7) & ~7u;
        uint64_t b = Read64(stackArg);
        stackArg += 8;
        double d;
        memcpy(&d, &b, 8);
        return d;
    };
    const char* f = (const char*)GuestPtr(fmtAddr);
    for (int guard = 0; *f && guard < 4096; guard++, f++)
    {
        if (*f != '%')
        {
            out += *f;
            continue;
        }
        std::string spec = "%";
        f++;
        while (*f && strchr("-+ #0123456789.*", *f))
        {
            if (*f == '*') spec += std::to_string((int32_t)nextInt());
            else spec += *f;
            f++;
        }
        bool isLong = false;
        while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'j' || *f == 't')
        {
            if (*f == 'l') isLong = true;
            f++;
        }
        char conv = *f;
        char buf[512];
        switch (conv)
        {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            spec += conv;
            snprintf(buf, sizeof(buf), spec.c_str(), nextInt());
            out += buf;
            break;
        case 'p':
            snprintf(buf, sizeof(buf), "%08x", nextInt());
            out += buf;
            break;
        case 's':
        {
            uint32_t a = nextInt();
            spec += 's';
            snprintf(buf, sizeof(buf), spec.c_str(), a ? (const char*)GuestPtr(a) : "(null)");
            out += buf;
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            spec += conv;
            snprintf(buf, sizeof(buf), spec.c_str(), nextDouble());
            out += buf;
            break;
        case '%':
            out += '%';
            break;
        case 0:
            return out;
        default:
            out += spec;
            out += conv;
            break;
        }
    }
    return out;
}

static void TrimNewline(std::string& s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
}

// OSReport(const char* fmt, ...)
PPC_FUNC(fn_8053E07C)
{
    std::string s = GuestFormat(ctx, ctx.r[3], 4);
    TrimNewline(s);
    if (!s.empty()) LOG_INFO("[OSReport] {}", s);
}

// OSPanic(const char* file, int line, const char* fmt, ...)
PPC_FUNC(fn_8053E10C)
{
    std::string s = GuestFormat(ctx, ctx.r[5], 6);
    TrimNewline(s);
    FATAL("OSPanic {}:{}: {}", (const char*)GuestPtr(ctx.r[3]), ctx.r[4], s);
}

// OSFatal(GXColor fg, GXColor bg, const char* msg)
PPC_FUNC(fn_8054024C)
{
    std::string s = (const char*)GuestPtr(ctx.r[5]);
    FATAL("OSFatal (called from {:08x}): {}", ctx.lr, s);
}

// u32 VIGetRetraceCount(void). The game paces frames by spinning on this until enough
// retraces have passed; once the value stops changing, sleep until the next interrupt
// instead of burning a host core.
#include "scheduler.h"
PPC_FUNC(fn_80554C58)
{
    static uint32_t last = ~0u, repeats = 0;
    fn_80554C58_impl(ctx, base);
    if (ctx.r[3] != last)
    {
        last = ctx.r[3];
        repeats = 0;
    }
    else if (++repeats > 64 && (ctx.msr & 0x8000))
        Sched_IdleWait();
}
