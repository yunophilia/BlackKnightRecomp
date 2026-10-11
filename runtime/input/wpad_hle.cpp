// High-level replacement for the WPAD library (Wii Remote over Bluetooth).
//
// The Bluetooth stack is never started. Instead the WPAD API functions that KPAD
// and the game use are replaced: a Wii Remote + Nunchuk is reported on channel 0
// and its samples are synthesised from the host gamepad (see motion.cpp).
#include "../config.h"
#include "gamepad.h"
#include "motion.h"
#include "../kernel/memory.h"
#include "../cpu/cpu.h"
#include "../log.h"
#include "../../generated/ppc_recomp_shared.h"

#include <deque>
#include <windows.h>
#include <Xinput.h>

namespace
{
enum : int32_t
{
    WPAD_ERR_NONE = 0,
    WPAD_ERR_NO_CONTROLLER = -1,
    WPAD_ERR_BUSY = -2,
};
enum : uint32_t
{
    WPAD_DEV_CORE = 0,
    WPAD_DEV_FREESTYLE = 1,
    WPAD_DEV_CLASSIC = 2,
    WPAD_DEV_NOT_FOUND = 253,
};

struct Chan
{
    uint32_t samplingCb = 0;
    uint32_t connectCb = 0;
    uint32_t extensionCb = 0;
    bool connectNotified = false;
    bool extNotified = false;
    uint32_t dataFormat = 5;   // WPAD_FMT_FREESTYLE_ACC_DPD
    bool motor = false;
    uint64_t lastSampleTb = 0;
};

Chan s_chan[4];
bool s_wpadInit = false;

struct PendingCb
{
    uint32_t func;
    uint32_t r3, r4;
};
std::deque<PendingCb> s_pending;

bool Connected(int chan)
{
    if (chan != 0) return false;
    return true;   // always present: keyboard works even without a controller
}

void CallGuest(PPCContext& ctx, uint8_t* base, uint32_t func, uint32_t r3, uint32_t r4)
{
    PPCContext saved = ctx;
    ctx.r[1] -= 0x100;
    ctx.r[3] = r3;
    ctx.r[4] = r4;
    ctx.lr = 0;
    ppc_call_indirect(ctx, base, func);   // may live in the interpreted HOME menu module
    ctx = saved;
}
}   // namespace

// Runs pending connection callbacks and feeds samples to KPAD's sampling callback.
void WPAD_Pump(PPCContext& ctx, uint8_t* base)
{
    for (int c = 0; c < 4; c++)
    {
        Chan& ch = s_chan[c];
        if (!Connected(c)) continue;
        if (ch.connectCb && !ch.connectNotified)
        {
            ch.connectNotified = true;
            LOG_INFO("WPAD: reporting controller {} connected", c);
            CallGuest(ctx, base, ch.connectCb, c, (uint32_t)WPAD_ERR_NONE);
        }
        if (ch.extensionCb && !ch.extNotified)
        {
            ch.extNotified = true;
            LOG_INFO("WPAD: reporting Nunchuk on controller {}", c);
            CallGuest(ctx, base, ch.extensionCb, c, WPAD_DEV_FREESTYLE);
        }
    }
    while (!s_pending.empty())
    {
        PendingCb p = s_pending.front();
        s_pending.pop_front();
        CallGuest(ctx, base, p.func, p.r3, p.r4);
    }
    // Real Wii Remotes report at ~200 Hz; deliver the samples that are due.
    uint64_t now = Timebase_Now();
    uint64_t period = TB_FREQ / 200;
    for (int c = 0; c < 4; c++)
    {
        Chan& ch = s_chan[c];
        if (!ch.samplingCb || !Connected(c)) continue;
        if (ch.lastSampleTb == 0 || now - ch.lastSampleTb > period * 16) ch.lastSampleTb = now - period;
        int n = 0;
        while (ch.lastSampleTb + period <= now && n < 16)
        {
            ch.lastSampleTb += period;
            Motion_Step(c, 1.0f / 200.0f);
            CallGuest(ctx, base, ch.samplingCb, c, 0);
            n++;
        }
    }
}

// ------------------------------------------------------------------ overrides
// WPADInit
PPC_FUNC(fn_803D3750)
{
    s_wpadInit = true;
    LOG_INFO("WPAD: init (Bluetooth stack replaced by HLE)");
}

// WPADGetStatus -> WPAD_STATE_SETUP
PPC_FUNC(fn_803D37D0)
{
    ctx.r[3] = 3;
}

// s32 WPADProbe(s32 chan, u32* type)
PPC_FUNC(fn_803D4B58)
{
    int chan = (int)ctx.r[3];
    uint32_t typePtr = ctx.r[4];
    if (chan < 0 || chan > 3 || !Connected(chan))
    {
        if (typePtr) Write32(typePtr, WPAD_DEV_NOT_FOUND);
        ctx.r[3] = (uint32_t)WPAD_ERR_NO_CONTROLLER;
        return;
    }
    if (typePtr) Write32(typePtr, WPAD_DEV_FREESTYLE);
    ctx.r[3] = WPAD_ERR_NONE;
}

// WPADSamplingCallback WPADSetSamplingCallback(s32 chan, WPADSamplingCallback cb)
PPC_FUNC(fn_803D4BE4)
{
    int chan = (int)ctx.r[3] & 3;
    uint32_t old = s_chan[chan].samplingCb;
    s_chan[chan].samplingCb = ctx.r[4];
    ctx.r[3] = old;
}

// WPADConnectCallback WPADSetConnectCallback(s32 chan, WPADConnectCallback cb)
PPC_FUNC(fn_803D4C6C)
{
    int chan = (int)ctx.r[3] & 3;
    uint32_t old = s_chan[chan].connectCb;
    s_chan[chan].connectCb = ctx.r[4];
    s_chan[chan].connectNotified = false;
    ctx.r[3] = old;
}

// WPADExtensionCallback WPADSetExtensionCallback(s32 chan, WPADExtensionCallback cb)
PPC_FUNC(fn_803D4CF4)
{
    int chan = (int)ctx.r[3] & 3;
    uint32_t old = s_chan[chan].extensionCb;
    s_chan[chan].extensionCb = ctx.r[4];
    s_chan[chan].extNotified = false;
    ctx.r[3] = old;
}

// void WPADRead(s32 chan, void* status)
PPC_FUNC(fn_803D5488)
{
    int chan = (int)ctx.r[3];
    uint32_t st = ctx.r[4];
    if (chan < 0 || chan > 3 || !st) return;
    Motion_FillStatus(chan, st, Connected(chan));
}

// void WPADGetAccGravityUnit(s32 chan, u32 type, WPADAcc* acc)
PPC_FUNC(fn_803D4910)
{
    uint32_t acc = ctx.r[5];
    if (!acc) return;
    int16_t g = (int16_t)Motion_GravityUnit(ctx.r[4]);
    Write16(acc, (uint16_t)g);
    Write16(acc + 2, (uint16_t)g);
    Write16(acc + 4, (uint16_t)g);
}

// void WPADControlMotor(s32 chan, u32 command)
PPC_FUNC(fn_803D50C0)
{
    int chan = (int)ctx.r[3];
    bool on = ctx.r[4] != 0;
    if (chan < 0 || chan > 3) return;
    if (!g_config.rumble) on = false;
    if (s_chan[chan].motor != on)
    {
        s_chan[chan].motor = on;
        XINPUT_VIBRATION v{};
        v.wLeftMotorSpeed = on ? 30000 : 0;
        v.wRightMotorSpeed = on ? 20000 : 0;
        XInputSetState(chan, &v);
    }
}

// s32 WPADGetInfoAsync(s32 chan, WPADInfo* info, WPADCallback cb)
PPC_FUNC(fn_803D4EA0)
{
    int chan = (int)ctx.r[3];
    uint32_t info = ctx.r[4], cb = ctx.r[5];
    if (chan < 0 || chan > 3 || !Connected(chan))
    {
        ctx.r[3] = (uint32_t)WPAD_ERR_NO_CONTROLLER;
        return;
    }
    if (info)
    {
        // dpd, speaker, attach, lowBat, nearempty, battery, led, protocol, firmware
        Write32(info + 0, 1);
        Write32(info + 4, 0);
        Write32(info + 8, 1);
        Write32(info + 12, 0);
        Write32(info + 16, 0);
        Write8(info + 20, 4);
        Write8(info + 21, (uint8_t)(1 << chan));
        Write8(info + 22, 0);
        Write8(info + 23, 0);
    }
    if (cb) s_pending.push_back({ cb, (uint32_t)chan, (uint32_t)WPAD_ERR_NONE });
    ctx.r[3] = WPAD_ERR_NONE;
}

// u32 WPADGetDataFormat(s32 chan) (Dolphin's signature DB calls it WPADIsDpdEnabled)
PPC_FUNC(fn_803D4D50)
{
    ctx.r[3] = s_chan[ctx.r[3] & 3].dataFormat;
}
// BOOL WPADIsDpdEnabled(s32 chan)
PPC_FUNC(fn_803D73C4) { ctx.r[3] = 1; }

// s32 WPADDisconnect(s32 chan)
PPC_FUNC(fn_803D4A0C)
{
    LOG_INFO("WPAD: game requested disconnect of {}", ctx.r[3]);
    ctx.r[3] = WPAD_ERR_NONE;
}

// s32 WPADControlSpeaker(s32 chan, u32 command, WPADCallback cb)
PPC_FUNC(fn_803D5924)
{
    uint32_t chan = ctx.r[3], cb = ctx.r[5];
    if (cb) s_pending.push_back({ cb, chan, (uint32_t)WPAD_ERR_NONE });
    ctx.r[3] = WPAD_ERR_NONE;
}

// BOOL WPADCanSendStreamData(s32 chan)
PPC_FUNC(fn_803D70FC) { ctx.r[3] = 0; }

// s32 WPADSendStreamData(s32 chan, void* p, u16 len)
PPC_FUNC(fn_803D7180) { ctx.r[3] = WPAD_ERR_NONE; }

// u8 WPADGetRadioSensitivity(s32 chan)
PPC_FUNC(fn_803D37D4) { ctx.r[3] = 100; }

// KPADReadEx-like core read (0x80574558): deliver due samples before KPAD reads.
PPC_FUNC(fn_80574558)
{
    WPAD_Pump(ctx, base);
    fn_80574558_impl(ctx, base);
}

// s32 WPADSetDataFormat(s32 chan, u32 fmt)
PPC_FUNC(fn_803D4D94)
{
    int chan = (int)ctx.r[3];
    if (chan < 0 || chan > 3 || !Connected(chan)) { ctx.r[3] = (uint32_t)WPAD_ERR_NO_CONTROLLER; return; }
    if (s_chan[chan].dataFormat != ctx.r[4])
        LOG_INFO("WPAD: channel {} data format {}", chan, ctx.r[4]);
    s_chan[chan].dataFormat = ctx.r[4];
    ctx.r[3] = WPAD_ERR_NONE;
}

// s32 WPADControlDpd(s32 chan, u32 command, WPADCallback cb)
PPC_FUNC(fn_803D7498)
{
    uint32_t chan = ctx.r[3], cb = ctx.r[5];
    if (cb) s_pending.push_back({ cb, chan, (uint32_t)WPAD_ERR_NONE });
    ctx.r[3] = WPAD_ERR_NONE;
}

// s32 (WPAD control with callback, e.g. MotionPlus/BLC control) - acknowledge immediately
PPC_FUNC(fn_803D8518)
{
    uint32_t chan = ctx.r[3], cb = ctx.r[5];
    if (cb) s_pending.push_back({ cb, chan, (uint32_t)WPAD_ERR_NONE });
    ctx.r[3] = WPAD_ERR_NONE;
}
