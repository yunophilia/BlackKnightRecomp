// DSP interface (mailboxes, control, ARAM/AI DMA) with HLE microcodes.
#include "hw.h"
#include "dsp.h"
#include "../cpu/cpu.h"
#include "../kernel/memory.h"
#include "../kernel/scheduler.h"
#include "../audio/ax.h"
#include "../audio/audio_out.h"
#include "../log.h"

#include <deque>

// ------------------------------------------------------------------ mail handler
struct Mail { uint32_t value; bool interrupt; };
static std::deque<Mail> s_mails;
static uint32_t s_lastMail = 0;
static bool s_halted = true;
static uint32_t s_cpuMailbox = 0;

static uint16_t s_control = 0x0804;   // halt | init
enum : uint16_t
{
    CTL_RESET = 1 << 0, CTL_ASSERT_INT = 1 << 1, CTL_HALT = 1 << 2,
    CTL_AID = 1 << 3, CTL_AID_MASK = 1 << 4, CTL_ARAM = 1 << 5, CTL_ARAM_MASK = 1 << 6,
    CTL_DSP = 1 << 7, CTL_DSP_MASK = 1 << 8, CTL_DMA_STATE = 1 << 9, CTL_INIT_CODE = 1 << 10, CTL_INIT = 1 << 11,
};
static uint64_t s_initCodeClearTime = 0;

static int s_evtDspInt = -1;
static int s_evtAIDma = -1;

static void UpdateInterrupts()
{
    bool any = ((s_control & CTL_AID) && (s_control & CTL_AID_MASK)) ||
               ((s_control & CTL_ARAM) && (s_control & CTL_ARAM_MASK)) ||
               ((s_control & CTL_DSP) && (s_control & CTL_DSP_MASK));
    PI_SetInterrupt(INT_CAUSE_DSP, any);
}

static void GenerateDSPInterrupt(uint64_t delayTicks = 0)
{
    if (delayTicks == 0)
    {
        s_control |= CTL_DSP;
        UpdateInterrupts();
    }
    else
    {
        Sched_ScheduleIn(s_evtDspInt, delayTicks);
    }
}

void DSP_PushMail(uint32_t mail, bool interrupt, uint64_t delayTicks)
{
    if (interrupt)
    {
        if (s_mails.empty())
            GenerateDSPInterrupt(delayTicks);
        else
            s_mails.front().interrupt = true;
    }
    s_mails.push_back({ mail, false });
}

static uint16_t ReadMailHigh()
{
    if (!s_halted && !s_mails.empty())
        s_lastMail = s_mails.front().value;
    return (uint16_t)(s_lastMail >> 16);
}

static uint16_t ReadMailLow()
{
    if (!s_halted && !s_mails.empty())
    {
        s_lastMail = s_mails.front().value;
        bool irq = s_mails.front().interrupt;
        s_mails.pop_front();
        if (irq)
            GenerateDSPInterrupt();
    }
    s_lastMail &= ~0x80000000u;
    return (uint16_t)s_lastMail;
}

// ------------------------------------------------------------------ microcodes
enum class UCode { ROM, INIT, AX };
static UCode s_ucode = UCode::ROM;

// ROM ucode boot parameters
static uint32_t s_romNextParam = 0;
static uint32_t s_romRamAddr, s_romLength, s_romImemAddr, s_romStartPc;

// AX state
enum class AXMail { CmdListSize, CmdListAddr, NextTask };
static AXMail s_axState = AXMail::CmdListSize;
static uint16_t s_axCmdListSize = 0;
static bool s_axUploadInProgress = false;
static int s_axUploadStep = 0;

static void SetUCode(UCode u)
{
    s_mails.clear();
    s_ucode = u;
    switch (u)
    {
    case UCode::ROM:
        s_romNextParam = 0;
        DSP_PushMail(0x8071FEED, false, 0);
        break;
    case UCode::INIT:
        DSP_PushMail(0x80544348, false, 0);
        break;
    case UCode::AX:
        s_axState = AXMail::CmdListSize;
        s_axUploadInProgress = false;
        AX_Reset();
        DSP_PushMail(0xDCD10000, true, 0);   // DSP_INIT
        break;
    }
}

static void HandleMail(uint32_t mail)
{
    switch (s_ucode)
    {
    case UCode::ROM:
        if (s_romNextParam == 0)
        {
            if ((mail & 0xFFFF0000) != 0x80F30000)
                DSP_PushMail(0xFEEE0000 | (mail & 0xFFFF), false, 0);
            else
                s_romNextParam = mail;
        }
        else
        {
            switch (s_romNextParam)
            {
            case 0x80F3A001: s_romRamAddr = mail; break;
            case 0x80F3A002: s_romLength = mail & 0xFFFF; break;
            case 0x80F3C002: s_romImemAddr = mail & 0xFFFF; break;
            case 0x80F3D001:
                s_romStartPc = mail & 0xFFFF;
            {
                // Ector CRC of the uploaded IRAM image identifies the AX version
                const uint8_t* p = PhysPtr(s_romRamAddr);
                uint32_t crc = 0;
                for (uint32_t i = 0; i < s_romLength; i++)
                {
                    crc ^= p[i];
                    crc = (crc << 3) | (crc >> 29);
                }
                LOG_INFO("DSP: booting ucode from {:08x} len {:04x} crc {:08x} -> AX HLE", s_romRamAddr, s_romLength, crc);
                AX_SetUCodeCRC(crc);
                SetUCode(UCode::AX);
            }
                return;
            }
            s_romNextParam = 0;
        }
        break;
    case UCode::INIT:
        break;
    case UCode::AX:
        if (s_axUploadInProgress)
        {
            if (++s_axUploadStep >= 10)
            {
                s_axUploadInProgress = false;
                LOG_WARN("DSP: game switched ucode (ignored, staying on AX)");
                DSP_PushMail(0xDCD10001, true, 0);   // DSP_RESUME
            }
            return;
        }
        switch (s_axState)
        {
        case AXMail::CmdListSize:
            if ((mail & 0xFFFF0000) == 0xBABE0000)
            {
                s_axCmdListSize = (uint16_t)mail;
                s_axState = AXMail::CmdListAddr;
            }
            else
                LOG_WARN("AX: unexpected mail {:08x}", mail);
            break;
        case AXMail::CmdListAddr:
            AX_ProcessCommandList(mail, s_axCmdListSize);
            // signal end of work (DSP_YIELD) a little later, as real hardware does
            DSP_PushMail(0xDCD10002, true, 2500 * TB_FREQ / 486000000ull + 50);
            s_axState = AXMail::NextTask;
            break;
        case AXMail::NextTask:
            switch (mail & 0xFFFF)
            {
            case 0: DSP_PushMail(0xDCD10001, true, 0); s_axState = AXMail::CmdListSize; break;   // resume
            case 1: s_axUploadInProgress = true; s_axUploadStep = 0; s_axState = AXMail::CmdListSize; break;
            case 2: SetUCode(UCode::ROM); break;
            case 3: s_axState = AXMail::CmdListSize; break;   // continue
            default: LOG_WARN("AX: unknown task mail {:08x}", mail); break;
            }
            break;
        }
        break;
    }
}

// ------------------------------------------------------------------ ARAM / AI DMA
static uint32_t s_aramMM, s_aramAR, s_aramCount;
static uint16_t s_aramSize, s_aramMode, s_aramRefresh;
static uint32_t s_aiDmaStart;      // physical
static uint16_t s_aiDmaCtrl;       // bit15 enable, length in 32-byte blocks
static uint32_t s_aiDmaBlocksLeft;
static uint32_t s_aiCurAddr;
static uint32_t s_aiSampleRate = 32000;

static void AIDmaEvent(uint64_t, int64_t)
{
    if (!(s_aiDmaCtrl & 0x8000))
        return;
    // One full buffer has been played; hand it to the mixer and restart.
    uint32_t len = (s_aiDmaCtrl & 0x7FFF) * 32;
    AudioOut_PushDSP(PhysPtr(s_aiCurAddr), len, s_aiSampleRate);
    s_aiCurAddr = s_aiDmaStart;
    s_aiDmaBlocksLeft = s_aiDmaCtrl & 0x7FFF;
    s_control |= CTL_AID;
    UpdateInterrupts();
    uint64_t ticks = (uint64_t)len / 4 * TB_FREQ / s_aiSampleRate;
    Sched_ScheduleIn(s_evtAIDma, ticks ? ticks : 1000);
}

static void DspIntEvent(uint64_t, int64_t)
{
    s_control |= CTL_DSP;
    UpdateInterrupts();
}

void DSP_SetAISampleRate(uint32_t rate) { s_aiSampleRate = rate; }

// ------------------------------------------------------------------ registers
uint16_t DSP_Read16(uint32_t off)
{
    switch (off)
    {
    case 0x00: return (uint16_t)(s_cpuMailbox >> 16);
    case 0x02: return (uint16_t)s_cpuMailbox;
    case 0x04: return ReadMailHigh();
    case 0x06: return ReadMailLow();
    case 0x0A:
        if ((s_control & CTL_INIT_CODE) && Timebase_Now() >= s_initCodeClearTime)
            s_control &= ~CTL_INIT_CODE;
        return s_control;
    case 0x12: return s_aramSize;
    case 0x16: return s_aramMode | 1;   // ARAM init done
    case 0x1A: return s_aramRefresh;
    case 0x20: return (uint16_t)(s_aramMM >> 16);
    case 0x22: return (uint16_t)s_aramMM;
    case 0x24: return (uint16_t)(s_aramAR >> 16);
    case 0x26: return (uint16_t)s_aramAR;
    case 0x28: return (uint16_t)(s_aramCount >> 16);
    case 0x2A: return (uint16_t)s_aramCount;
    case 0x30: return (uint16_t)(s_aiDmaStart >> 16);
    case 0x32: return (uint16_t)s_aiDmaStart;
    case 0x36: return s_aiDmaCtrl;
    case 0x3A: return (uint16_t)s_aiDmaBlocksLeft;
    default:
        LOG_WARN("DSP read16 {:02x}", off);
        return 0;
    }
}

void DSP_Write16(uint32_t off, uint16_t v)
{
    switch (off)
    {
    case 0x00: s_cpuMailbox = (s_cpuMailbox & 0xFFFF) | ((uint32_t)v << 16); break;
    case 0x02:
        s_cpuMailbox = (s_cpuMailbox & 0xFFFF0000) | v;
        HandleMail(s_cpuMailbox);
        s_cpuMailbox &= 0x7FFFFFFF;
        break;
    case 0x0A:
    {
        uint16_t t = v;
        if ((s_control & CTL_HALT) != (t & CTL_HALT))
            s_halted = (t & CTL_HALT) != 0;
        if (t & CTL_RESET)
        {
            SetUCode(UCode::ROM);
            t &= ~CTL_RESET;
        }
        if ((s_control & CTL_INIT) && !(t & CTL_INIT))
        {
            SetUCode(UCode::INIT);
            t |= CTL_INIT_CODE;
            s_initCodeClearTime = Timebase_Now() + 130;
        }
        // interrupt status bits are write-1-to-clear
        uint16_t ints = CTL_AID | CTL_ARAM | CTL_DSP;
        uint16_t newCtl = (s_control & ints & ~(t & ints)) | (t & ~ints);
        newCtl &= ~CTL_ASSERT_INT;
        s_control = newCtl;
        UpdateInterrupts();
        break;
    }
    case 0x12: s_aramSize = v; break;
    case 0x16: s_aramMode = v; break;
    case 0x1A: s_aramRefresh = v; break;
    case 0x20: s_aramMM = (s_aramMM & 0xFFFF) | ((uint32_t)v << 16); break;
    case 0x22: s_aramMM = (s_aramMM & 0xFFFF0000) | (v & ~31u); break;
    case 0x24: s_aramAR = (s_aramAR & 0xFFFF) | ((uint32_t)v << 16); break;
    case 0x26: s_aramAR = (s_aramAR & 0xFFFF0000) | (v & ~31u); break;
    case 0x28: s_aramCount = (s_aramCount & 0xFFFF) | ((uint32_t)v << 16); break;
    case 0x2A:
        s_aramCount = (s_aramCount & 0xFFFF0000) | (v & ~31u);
        // ARAM DMA completes instantly (Wii has no ARAM; nothing to copy)
        s_control |= CTL_ARAM;
        UpdateInterrupts();
        break;
    case 0x30: s_aiDmaStart = (s_aiDmaStart & 0xFFFF) | ((uint32_t)(v & 0x1FFF) << 16); break;
    case 0x32: s_aiDmaStart = (s_aiDmaStart & 0xFFFF0000) | (v & ~31u); break;
    case 0x36:
    {
        bool wasOn = s_aiDmaCtrl & 0x8000;
        s_aiDmaCtrl = v;
        if ((v & 0x8000) && !wasOn)
        {
            s_aiCurAddr = s_aiDmaStart;
            s_aiDmaBlocksLeft = v & 0x7FFF;
            uint32_t len = (v & 0x7FFF) * 32;
            uint64_t ticks = (uint64_t)len / 4 * TB_FREQ / s_aiSampleRate;
            Sched_Cancel(s_evtAIDma);
            Sched_ScheduleIn(s_evtAIDma, ticks ? ticks : 1000);
        }
        else if (!(v & 0x8000))
            Sched_Cancel(s_evtAIDma);
        break;
    }
    default:
        LOG_WARN("DSP write16 {:02x} = {:04x}", off, v);
        break;
    }
}

uint32_t DSP_Read32(uint32_t off) { return ((uint32_t)DSP_Read16(off) << 16) | DSP_Read16(off + 2); }
void DSP_Write32(uint32_t off, uint32_t v) { DSP_Write16(off, (uint16_t)(v >> 16)); DSP_Write16(off + 2, (uint16_t)v); }

void DSP_Init()
{
    s_evtDspInt = Sched_RegisterEvent("DSPInt", DspIntEvent);
    s_evtAIDma = Sched_RegisterEvent("AIDMA", AIDmaEvent);
    SetUCode(UCode::ROM);
    s_halted = true;
}

// ------------------------------------------------------------------ AI (streaming interface)
static uint32_t s_aiControl = 0, s_aiVolume = 0, s_aiSampleCounter = 0, s_aiIntTiming = 0;
static uint64_t s_aiCounterBaseTb = 0;   // timebase when the counter was last latched

// The streaming sample counter advances at the AIS rate (bit 1: 0 = 32 kHz, 1 = 48 kHz) while PSTAT is set.
static uint32_t AICounterNow()
{
    if (!(s_aiControl & 1)) return s_aiSampleCounter;
    uint32_t rate = (s_aiControl & 2) ? 48000 : 32000;
    uint64_t elapsed = Timebase_Now() - s_aiCounterBaseTb;
    return s_aiSampleCounter + (uint32_t)(elapsed * rate / TB_FREQ);
}

static void AILatchCounter()
{
    s_aiSampleCounter = AICounterNow();
    s_aiCounterBaseTb = Timebase_Now();
}

uint32_t AI_Read32(uint32_t off)
{
    switch (off)
    {
    case 0x00: return s_aiControl;
    case 0x04: return s_aiVolume;
    case 0x08: return AICounterNow();
    case 0x0C: return s_aiIntTiming;
    }
    return 0;
}

void AI_Write32(uint32_t off, uint32_t v)
{
    switch (off)
    {
    case 0x00:
        // bit 0: PSTAT, bit 1: AIS rate, bit 5: reset counter, bit 6: DSP rate (1 = 32 kHz)
        AILatchCounter();
        s_aiControl = v & ~0x28u;   // ack bits
        DSP_SetAISampleRate((v & 0x40) ? 32000 : 48000);
        if (v & 0x20) s_aiSampleCounter = 0;
        s_aiCounterBaseTb = Timebase_Now();
        break;
    case 0x04: s_aiVolume = v; break;
    case 0x08: s_aiSampleCounter = v; s_aiCounterBaseTb = Timebase_Now(); break;
    case 0x0C: s_aiIntTiming = v; break;
    }
}
