// GX command processor: CP/PE registers, write-gather pipe and opcode decoding.
#include "gx_state.h"
#include "../hw/hw.h"
#include "../kernel/memory.h"
#include "../log.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>

GXState g_gx;

uint32_t PI_GetFifoWptr();
void PI_SetFifoWptr(uint32_t v);
uint32_t PI_GetFifoBase();
uint32_t PI_GetFifoEnd();

// ------------------------------------------------------------------ vertex layout
static const uint8_t kCompSize[8] = { 1, 1, 2, 2, 4, 0, 0, 0 };
static const uint8_t kColorSize[8] = { 2, 3, 4, 2, 3, 4, 0, 0 };

void GX_ComputeLayout(int vat, VertexLayout& l)
{
    uint32_t lo = g_gx.cp[CP_VCD_LO];
    uint32_t hi = g_gx.cp[CP_VCD_HI];
    uint32_t a = g_gx.cp[CP_VAT_A + vat];
    uint32_t b = g_gx.cp[CP_VAT_B + vat];
    uint32_t c = g_gx.cp[CP_VAT_C + vat];

    l.posMtxIdx = lo & 1;
    for (int i = 0; i < 8; i++) l.texMtxIdx[i] = (lo >> (1 + i)) & 1;
    l.pos = (lo >> 9) & 3;
    l.nrm = (lo >> 11) & 3;
    l.col[0] = (lo >> 13) & 3;
    l.col[1] = (lo >> 15) & 3;
    for (int i = 0; i < 8; i++) l.tex[i] = (hi >> (2 * i)) & 3;

    l.posCnt = a & 1;
    l.posFmt = (a >> 1) & 7;
    l.posShift = (a >> 4) & 31;
    l.nrmCnt = (a >> 9) & 1;
    l.nrmFmt = (a >> 10) & 7;
    l.colCnt[0] = (a >> 13) & 1;
    l.colFmt[0] = (a >> 14) & 7;
    l.colCnt[1] = (a >> 17) & 1;
    l.colFmt[1] = (a >> 18) & 7;
    l.texCnt[0] = (a >> 21) & 1;
    l.texFmt[0] = (a >> 22) & 7;
    l.texShift[0] = (a >> 25) & 31;
    l.byteDequant = (a >> 30) & 1;
    l.nrmIndex3 = (a >> 31) & 1;
    l.texCnt[1] = b & 1; l.texFmt[1] = (b >> 1) & 7; l.texShift[1] = (b >> 4) & 31;
    l.texCnt[2] = (b >> 9) & 1; l.texFmt[2] = (b >> 10) & 7; l.texShift[2] = (b >> 13) & 31;
    l.texCnt[3] = (b >> 18) & 1; l.texFmt[3] = (b >> 19) & 7; l.texShift[3] = (b >> 22) & 31;
    l.texCnt[4] = (b >> 27) & 1; l.texFmt[4] = (b >> 28) & 7; l.texShift[4] = c & 31;
    l.texCnt[5] = (c >> 5) & 1; l.texFmt[5] = (c >> 6) & 7; l.texShift[5] = (c >> 9) & 31;
    l.texCnt[6] = (c >> 14) & 1; l.texFmt[6] = (c >> 15) & 7; l.texShift[6] = (c >> 18) & 31;
    l.texCnt[7] = (c >> 23) & 1; l.texFmt[7] = (c >> 24) & 7; l.texShift[7] = (c >> 27) & 31;

    uint32_t size = 0;
    size += l.posMtxIdx;
    for (int i = 0; i < 8; i++) size += l.texMtxIdx[i];
    auto attrSize = [](uint8_t type, uint32_t direct) -> uint32_t {
        switch (type)
        {
        case 1: return direct;
        case 2: return 1;
        case 3: return 2;
        default: return 0;
        }
    };
    size += attrSize(l.pos, kCompSize[l.posFmt] * (l.posCnt ? 3 : 2));
    if (l.nrm)
    {
        uint32_t n = l.nrmCnt ? 9 : 3;
        if (l.nrm == 1)
            size += kCompSize[l.nrmFmt] * n;
        else
            size += (l.nrm == 2 ? 1 : 2) * ((l.nrmCnt && l.nrmIndex3) ? 3 : 1);
    }
    for (int i = 0; i < 2; i++) size += attrSize(l.col[i], kColorSize[l.colFmt[i]]);
    for (int i = 0; i < 8; i++) size += attrSize(l.tex[i], kCompSize[l.texFmt[i]] * (l.texCnt[i] ? 2 : 1));
    l.size = size;
}

// ------------------------------------------------------------------ PE registers
static uint16_t s_peRegs[0x40];
static uint16_t s_peToken = 0;
static bool s_peTokenInt = false, s_peFinishInt = false;
static uint16_t s_peIntCtrl = 0;   // bit0 token enable, bit1 finish enable
// PE flags are set by the GPU thread and acknowledged by the CPU thread
static std::recursive_mutex s_peMutex;

static void PE_UpdateInterrupts()
{
    std::lock_guard lk(s_peMutex);
    PI_SetInterrupt(INT_CAUSE_PE_TOKEN, s_peTokenInt && (s_peIntCtrl & 1));
    PI_SetInterrupt(INT_CAUSE_PE_FINISH, s_peFinishInt && (s_peIntCtrl & 2));
}

uint16_t PE_Read16(uint32_t off)
{
    switch (off)
    {
    case 0x0A:
        return (s_peIntCtrl & 3) | (s_peTokenInt ? 4 : 0) | (s_peFinishInt ? 8 : 0);
    case 0x0E:
        return s_peToken;
    default:
        if (off >= 0x10 && off < 0x30) return 0;   // perf counters
        return s_peRegs[(off / 2) & 0x3F];
    }
}

void PE_Write16(uint32_t off, uint16_t v)
{
    switch (off)
    {
    case 0x0A:
    {
        std::lock_guard lk(s_peMutex);
        s_peIntCtrl = v & 3;
        if (v & 4) s_peTokenInt = false;
        if (v & 8) s_peFinishInt = false;
        PE_UpdateInterrupts();
        break;
    }
    case 0x0E:
        s_peToken = v;
        break;
    default:
        s_peRegs[(off / 2) & 0x3F] = v;
        break;
    }
}

// ------------------------------------------------------------------ CP registers (memory-mapped)
static uint16_t s_cpRegs[0x40];
static uint32_t s_cpFifoBase, s_cpFifoEnd, s_cpHiWM, s_cpLoWM, s_cpRWDist, s_cpWptr, s_cpRptr, s_cpBreak;

std::unique_lock<std::mutex> GX_SyncGPU();

uint16_t CP_Read16(uint32_t off)
{
    // status and FIFO pointers describe GPU progress: let it catch up first
    if (off == 0x00 || (off >= 0x30 && off <= 0x3A)) GX_SyncGPU();
    switch (off)
    {
    case 0x00:   // status: FIFO empty, GP idle for commands/reads
        return 0x0001 | 0x0004 | 0x0008 | 0x0010 | ((s_cpRegs[1] & 0) ? 0 : 0) | 0x000C;
    case 0x02: return s_cpRegs[1];
    case 0x20: return (uint16_t)s_cpFifoBase;
    case 0x22: return (uint16_t)(s_cpFifoBase >> 16);
    case 0x24: return (uint16_t)s_cpFifoEnd;
    case 0x26: return (uint16_t)(s_cpFifoEnd >> 16);
    case 0x28: return (uint16_t)s_cpHiWM;
    case 0x2A: return (uint16_t)(s_cpHiWM >> 16);
    case 0x2C: return (uint16_t)s_cpLoWM;
    case 0x2E: return (uint16_t)(s_cpLoWM >> 16);
    case 0x30: return 0;   // RW distance (we process synchronously)
    case 0x32: return 0;
    case 0x34: return (uint16_t)s_cpWptr;
    case 0x36: return (uint16_t)(s_cpWptr >> 16);
    case 0x38: return (uint16_t)s_cpWptr;   // read pointer == write pointer
    case 0x3A: return (uint16_t)(s_cpWptr >> 16);
    case 0x3C: return (uint16_t)s_cpBreak;
    case 0x3E: return (uint16_t)(s_cpBreak >> 16);
    default:
        return s_cpRegs[(off / 2) & 0x3F];
    }
}

void CP_Write16(uint32_t off, uint16_t v)
{
    auto lo = [&](uint32_t& r) { r = (r & 0xFFFF0000) | v; };
    auto hi = [&](uint32_t& r) { r = (r & 0xFFFF) | ((uint32_t)v << 16); };
    switch (off)
    {
    case 0x20: lo(s_cpFifoBase); break;
    case 0x22: hi(s_cpFifoBase); break;
    case 0x24: lo(s_cpFifoEnd); break;
    case 0x26: hi(s_cpFifoEnd); break;
    case 0x28: lo(s_cpHiWM); break;
    case 0x2A: hi(s_cpHiWM); break;
    case 0x2C: lo(s_cpLoWM); break;
    case 0x2E: hi(s_cpLoWM); break;
    case 0x30: lo(s_cpRWDist); break;
    case 0x32: hi(s_cpRWDist); break;
    case 0x34: lo(s_cpWptr); break;
    case 0x36: hi(s_cpWptr); break;
    case 0x38: lo(s_cpRptr); break;
    case 0x3A: hi(s_cpRptr); break;
    case 0x3C: lo(s_cpBreak); break;
    case 0x3E: hi(s_cpBreak); break;
    default:
        s_cpRegs[(off / 2) & 0x3F] = v;
        break;
    }
}

// ------------------------------------------------------------------ opcode decoder
static std::vector<uint8_t> s_fifo;    // pending bytes from the gather pipe
static size_t s_fifoPos = 0;

static inline uint16_t BE16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t BE32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static void LoadCPReg(uint8_t reg, uint32_t v)
{
    g_gx.cp[reg] = v;
    Render_OnCP(reg);
}

void Render_Flush();

static void LoadXF(uint32_t addr, uint32_t count, const uint8_t* data)
{
    Render_Flush();
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t a = addr + i;
        uint32_t v = BE32(data + 4 * i);
        if (a < 0x1000) g_gx.xfmem[a] = v;
        else if (a < 0x1100) g_gx.xfregs[a - 0x1000] = v;
    }
    Render_OnXF(addr, count);
}

static void LoadIndexedXF(int array, uint32_t word)
{
    uint32_t index = word >> 16;
    uint32_t size = ((word >> 12) & 0xF) + 1;
    uint32_t addr = word & 0xFFF;
    uint32_t base = g_gx.cp[CP_ARRAY_BASE + array];
    uint32_t stride = g_gx.cp[CP_ARRAY_STRIDE + array];
    const uint8_t* src = PhysPtr(base + index * stride);
    Render_Flush();
    for (uint32_t i = 0; i < size; i++)
        g_gx.xfmem[(addr + i) & 0xFFF] = BE32(src + 4 * i);
    Render_OnXF(addr, size);
}

void GX_PE_Finish()
{
    std::lock_guard lk(s_peMutex);
    s_peFinishInt = true;
    PE_UpdateInterrupts();
}

void GX_PE_Token(uint16_t token, bool interrupt)
{
    std::lock_guard lk(s_peMutex);
    s_peToken = token;
    if (interrupt)
    {
        s_peTokenInt = true;
        PE_UpdateInterrupts();
    }
}

static void LoadBP(uint32_t word)
{
    uint32_t reg = word >> 24;
    uint32_t val = word & 0xFFFFFF;
    uint32_t mask = g_gx.bpMaskValue;
    g_gx.bpMaskValue = 0xFFFFFF;
    if (reg == 0xFE)
    {
        g_gx.bpMaskValue = val;
        return;
    }
    uint32_t old = g_gx.bp[reg];
    uint32_t nv = (old & ~mask) | (val & mask);
    // pending geometry must be drawn with the state it was submitted under
    if (nv != old || reg == 0x52 || reg == 0x65 || (reg >= 0xE0 && reg <= 0xE7))
        Render_Flush();
    g_gx.bp[reg] = nv;
    switch (reg)
    {
    case 0x45:   // PE_DONE (draw done)
        if (nv & 2) GX_PE_Finish();
        break;
    case 0x47:   // PE token
        GX_PE_Token((uint16_t)nv, false);
        break;
    case 0x48:   // PE token + interrupt
        GX_PE_Token((uint16_t)nv, true);
        break;
    default:
        break;
    }
    Render_OnBP(reg, old, nv);
}

static size_t RunCommands(const uint8_t* p, size_t n, bool inDisplayList);

static void CallDisplayList(uint32_t addr, uint32_t size)
{
    const uint8_t* p = PhysPtr(addr);
    RunCommands(p, size, true);
}

// Returns number of bytes consumed (only complete commands are consumed).
static size_t RunCommands(const uint8_t* p, size_t n, bool inDisplayList)
{
    size_t pos = 0;
    VertexLayout layout;
    while (pos < n)
    {
        const uint8_t* c = p + pos;
        size_t left = n - pos;
        uint8_t op = c[0];
        size_t need;
        switch (op)
        {
        case 0x00:   // NOP
            pos += 1;
            continue;
        case 0x08:
            need = 6;
            if (left < need) return pos;
            LoadCPReg(c[1], BE32(c + 2));
            pos += need;
            continue;
        case 0x10:
        {
            if (left < 5) return pos;
            uint32_t w = BE32(c + 1);
            uint32_t count = ((w >> 16) & 0xF) + 1;
            need = 5 + 4 * (size_t)count;
            if (left < need) return pos;
            LoadXF(w & 0xFFFF, count, c + 5);
            pos += need;
            continue;
        }
        case 0x20: case 0x28: case 0x30: case 0x38:
            need = 5;
            if (left < need) return pos;
            LoadIndexedXF(12 + ((op >> 3) & 3), BE32(c + 1));
            pos += need;
            continue;
        case 0x40:
            need = 9;
            if (left < need) return pos;
            if (inDisplayList)
                LOG_WARN("GX: nested display list call ignored");
            else
                CallDisplayList(BE32(c + 1) & 0x1FFFFFE0, BE32(c + 5));
            pos += need;
            continue;
        case 0x44:   // unknown metrics
            pos += 1;
            continue;
        case 0x48:   // invalidate vertex cache
            pos += 1;
            continue;
        case 0x61:
            need = 5;
            if (left < need) return pos;
            LoadBP(BE32(c + 1));
            pos += need;
            continue;
        default:
            if (op & 0x80)
            {
                if (left < 3) return pos;
                int vat = op & 7;
                int prim = (op >> 3) & 7;
                uint32_t count = BE16(c + 1);
                {
                    // layouts only change with VCD/VAT writes; cache per VAT slot
                    struct Cached { uint32_t lo, hi, a, b, c; bool valid; VertexLayout l; };
                    static Cached cache[8];
                    Cached& ce = cache[vat];
                    uint32_t lo = g_gx.cp[CP_VCD_LO], hi = g_gx.cp[CP_VCD_HI];
                    uint32_t va = g_gx.cp[CP_VAT_A + vat], vb = g_gx.cp[CP_VAT_B + vat], vc = g_gx.cp[CP_VAT_C + vat];
                    if (!ce.valid || ce.lo != lo || ce.hi != hi || ce.a != va || ce.b != vb || ce.c != vc)
                    {
                        GX_ComputeLayout(vat, ce.l);
                        ce = { lo, hi, va, vb, vc, true, ce.l };
                    }
                    layout = ce.l;
                }
                need = 3 + (size_t)count * layout.size;
                if (left < need) return pos;
                if (count)
                    Render_Draw(prim, vat, count, c + 3, layout);
                pos += need;
                continue;
            }
            LOG_ERROR("GX: unknown opcode {:02x} (in {} at +{}), dropping {} bytes", op, inDisplayList ? "DL" : "FIFO", pos, left);
            return n;
        }
    }
    return pos;
}

static void ProcessFifo()
{
    size_t avail = s_fifo.size() - s_fifoPos;
    size_t used = RunCommands(s_fifo.data() + s_fifoPos, avail, false);
    s_fifoPos += used;
    if (s_fifoPos > 1 << 20 || s_fifoPos == s_fifo.size())
    {
        s_fifo.erase(s_fifo.begin(), s_fifo.begin() + s_fifoPos);
        s_fifoPos = 0;
    }
}

static inline void AdvanceWptr(uint32_t bytes)
{
    // Keep PI write pointer moving through the CPU FIFO so the SDK sees progress.
    uint32_t w = PI_GetFifoWptr() + bytes;
    uint32_t end = PI_GetFifoEnd();
    if (end && w >= end) w = PI_GetFifoBase() + (w - end);
    PI_SetFifoWptr(w);
}

// ------------------------------------------------------------------ GPU thread
// Like Dolphin's dual-core mode: the CPU thread only appends write-gather bytes to
// a lock-free ring; a separate thread parses them and renders. The CPU waits for
// the GPU only when it reads GPU state (CP status, EFB peeks) or the ring is full.
// PE draw-done/token interrupts are raised from the GPU thread as on hardware.
bool g_gpuThread = true;
static constexpr size_t RING_SIZE = 32u << 20;
static uint8_t* s_ring;
static size_t s_localW = 0;                     // CPU: bytes written
static std::atomic<size_t> s_ringW{ 0 };        // published to the GPU
static std::atomic<size_t> s_ringR{ 0 };        // consumed by the GPU
static std::atomic<bool> s_gpuSleeping{ false };
static HANDLE s_gpuWake, s_cpuWake;
static std::mutex s_gpuLock;                    // held while the GPU executes commands
static bool s_threadStarted = false;

static void GpuThread()
{
    SetThreadDescription(GetCurrentThread(), L"GX");
    for (;;)
    {
        size_t r = s_ringR.load(std::memory_order_relaxed);
        size_t w = s_ringW.load(std::memory_order_acquire);
        if (w == r)
        {
            SetEvent(s_cpuWake);
            // brief spin before sleeping: commands usually arrive in quick bursts
            for (int i = 0; i < 2000 && s_ringW.load(std::memory_order_acquire) == r; i++) YieldProcessor();
            if (s_ringW.load(std::memory_order_acquire) == r)
            {
                s_gpuSleeping.store(true);
                if (s_ringW.load(std::memory_order_acquire) == r) WaitForSingleObject(s_gpuWake, 2);
                s_gpuSleeping.store(false);
            }
            continue;
        }
        std::lock_guard lk(s_gpuLock);
        size_t n = w - r;
        size_t off = r % RING_SIZE;
        size_t first = std::min(n, RING_SIZE - off);
        s_fifo.insert(s_fifo.end(), s_ring + off, s_ring + off + first);
        if (first < n) s_fifo.insert(s_fifo.end(), s_ring, s_ring + (n - first));
        s_ringR.store(w, std::memory_order_release);
        ProcessFifo();
    }
}

static inline void Publish()
{
    s_ringW.store(s_localW, std::memory_order_release);
    if (s_gpuSleeping.load()) SetEvent(s_gpuWake);
}

// Blocks until the GPU thread has executed everything written so far. The returned
// lock keeps it idle, so the caller may touch renderer state (EFB peeks).
std::unique_lock<std::mutex> GX_SyncGPU()
{
    if (!s_threadStarted) return std::unique_lock<std::mutex>(s_gpuLock);
    Publish();
    while (s_ringR.load(std::memory_order_acquire) != s_localW)
    {
        SetEvent(s_gpuWake);
        WaitForSingleObject(s_cpuWake, 1);
    }
    return std::unique_lock<std::mutex>(s_gpuLock);
}

static uint32_t s_gatherCount = 0;

static inline void Gather(const uint8_t* b, int n)
{
    if (s_threadStarted)
    {
        while (s_localW + n - s_ringR.load(std::memory_order_acquire) > RING_SIZE)
        {
            Publish();
            SetEvent(s_gpuWake);
            WaitForSingleObject(s_cpuWake, 1);
        }
        size_t off = s_localW % RING_SIZE;
        if (off + n <= RING_SIZE) memcpy(s_ring + off, b, n);
        else for (int i = 0; i < n; i++) s_ring[(s_localW + i) % RING_SIZE] = b[i];
        s_localW += n;
    }
    else
        s_fifo.insert(s_fifo.end(), b, b + n);
    s_gatherCount += n;
    if (s_gatherCount >= 32)
    {
        AdvanceWptr(s_gatherCount & ~31u);
        s_gatherCount &= 31;
        if (s_threadStarted) Publish();
        else ProcessFifo();
    }
}

void GPFifo_Write8(uint8_t v) { Gather(&v, 1); }
void GPFifo_Write16(uint16_t v) { uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v }; Gather(b, 2); }
void GPFifo_Write32(uint32_t v) { uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; Gather(b, 4); }

// Called when the game explicitly flushes (e.g. GXFlush pads with 32 bytes); also useful before
// presenting.
void GX_FlushPipe()
{
    if (s_threadStarted) Publish();
    else ProcessFifo();
}

// Starts the GPU thread (after the renderer is initialised).
void GX_StartThread()
{
    if (!g_gpuThread || s_threadStarted) return;
    s_ring = (uint8_t*)VirtualAlloc(nullptr, RING_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    s_gpuWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s_cpuWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s_threadStarted = true;
    std::thread(GpuThread).detach();
    LOG_INFO("GX: running on a separate thread");
}

void GX_Init()
{
    memset(&g_gx, 0, sizeof(g_gx));
    g_gx.bpMaskValue = 0xFFFFFF;
    s_fifo.reserve(1 << 20);
}
