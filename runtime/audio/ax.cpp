// AX (Wii) DSP microcode HLE. Ported from Dolphin's AXWii/AXVoice/DSPAccelerator
// (GPL-2.0-or-later).
#include "ax.h"
#include "../hw/dsp.h"
#include "../kernel/memory.h"
#include "../log.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

namespace
{
// ---------------------------------------------------------------- structures (host order)
struct VolumeData { uint16_t volume, volume_delta; };
struct PBMixerWii
{
    VolumeData main_left, main_right, auxA_left, auxA_right, auxB_left, auxB_right, auxC_left, auxC_right;
    VolumeData main_surround, auxA_surround, auxB_surround, auxC_surround;
};
struct PBMixerWM { VolumeData main0, aux0, main1, aux1, main2, aux2, main3, aux3; };
struct PBInitialTimeDelay { uint16_t on, addrMemHigh, addrMemLow, offsetLeft, offsetRight, targetLeft, targetRight; };
struct PBUpdatesWii { uint16_t num_updates[3]; uint16_t data_hi, data_lo; };
struct PBDpopWii
{
    int16_t main_left, auxA_left, auxB_left, auxC_left, main_right, auxA_right, auxB_right, auxC_right;
    int16_t main_surround, auxA_surround, auxB_surround, auxC_surround;
};
struct PBDpopWM { int16_t main0, main1, main2, main3, aux0, aux1, aux2, aux3; };
struct PBVolumeEnvelope { int16_t cur_volume, cur_volume_delta; };
struct PBAudioAddr { uint16_t looping, sample_format, loop_addr_hi, loop_addr_lo, end_addr_hi, end_addr_lo, cur_addr_hi, cur_addr_lo; };
struct PBADPCMInfo { int16_t coefs[16]; uint16_t gain, pred_scale; int16_t yn1, yn2; };
struct PBSampleRateConverter { uint16_t ratio_hi, ratio_lo, cur_addr_frac; int16_t last_samples[4]; };
struct PBSampleRateConverterWM { uint16_t cur_addr_frac; int16_t last_samples[4]; };
struct PBADPCMLoopInfo { uint16_t pred_scale, yn1, yn2; };
struct PBLowPassFilter { uint16_t on; int16_t yn1; uint16_t a0; int16_t b0; };
struct PBHighPassFilter { uint16_t on; uint16_t unk[3]; };
struct PBBiquadFilter { uint16_t on; int16_t xn1, xn2, yn1, yn2, b0, b1, b2, a1, a2; };
union PBInfImpulseResponseWM { uint16_t on; PBLowPassFilter lpf; PBBiquadFilter biquad; };

struct AXPBWii
{
    uint16_t next_pb_hi, next_pb_lo, this_pb_hi, this_pb_lo;
    uint16_t src_type, coef_select, mixer_control_hi, mixer_control_lo;
    uint16_t running, is_stream;
    PBMixerWii mixer;
    PBInitialTimeDelay initial_time_delay;
    PBDpopWii dpop;
    PBUpdatesWii updates;
    PBVolumeEnvelope vol_env;
    PBAudioAddr audio_addr;
    PBADPCMInfo adpcm;
    PBSampleRateConverter src;
    PBADPCMLoopInfo adpcm_loop_info;
    PBLowPassFilter lpf;
    union
    {
        PBHighPassFilter hpf;
        PBBiquadFilter biquad;
    };
    uint16_t remote, remote_mixer_control;
    PBMixerWM remote_mixer;
    PBDpopWM remote_dpop;
    PBSampleRateConverterWM remote_src;
    PBInfImpulseResponseWM remote_iir;
    uint16_t pad[2];
};
static_assert(sizeof(AXPBWii) % 2 == 0, "PB must be u16 array");

enum { SRCTYPE_POLYPHASE = 0, SRCTYPE_LINEAR = 1, SRCTYPE_NEAREST = 2 };

enum AXMixControl : uint32_t
{
    MIX_MAIN_L = 0x000001, MIX_MAIN_L_RAMP = 0x000002, MIX_MAIN_R = 0x000004, MIX_MAIN_R_RAMP = 0x000008,
    MIX_MAIN_S = 0x000010, MIX_MAIN_S_RAMP = 0x000020, MIX_AUXA_L = 0x000040, MIX_AUXA_L_RAMP = 0x000080,
    MIX_AUXA_R = 0x000100, MIX_AUXA_R_RAMP = 0x000200, MIX_AUXA_S = 0x000400, MIX_AUXA_S_RAMP = 0x000800,
    MIX_AUXB_L = 0x001000, MIX_AUXB_L_RAMP = 0x002000, MIX_AUXB_R = 0x004000, MIX_AUXB_R_RAMP = 0x008000,
    MIX_AUXB_S = 0x010000, MIX_AUXB_S_RAMP = 0x020000, MIX_AUXC_L = 0x040000, MIX_AUXC_L_RAMP = 0x080000,
    MIX_AUXC_R = 0x100000, MIX_AUXC_R_RAMP = 0x200000, MIX_AUXC_S = 0x400000, MIX_AUXC_S_RAMP = 0x800000,
};

inline uint32_t HILO(uint16_t hi, uint16_t lo) { return ((uint32_t)hi << 16) | lo; }
inline int16_t ClampS16(int64_t s) { return (int16_t)std::clamp<int64_t>(s, -0x8000, 0x7FFF); }

// guest memory helpers (DSP addresses are physical)
inline uint8_t ReadARAM(uint32_t a) { return *PhysPtr(a); }
void CopyFromEmuSwapped16(uint16_t* dst, uint32_t addr, size_t bytes)
{
    const uint8_t* p = PhysPtr(addr);
    for (size_t i = 0; i < bytes / 2; i++) dst[i] = (uint16_t)((p[2 * i] << 8) | p[2 * i + 1]);
}
void CopyToEmuSwapped16(uint32_t addr, const uint16_t* src, size_t bytes)
{
    uint8_t* p = PhysPtr(addr);
    for (size_t i = 0; i < bytes / 2; i++) { p[2 * i] = (uint8_t)(src[i] >> 8); p[2 * i + 1] = (uint8_t)src[i]; }
}
void CopyToEmuSwapped32(uint32_t addr, const int* src, size_t bytes)
{
    uint8_t* p = PhysPtr(addr);
    for (size_t i = 0; i < bytes / 4; i++)
    {
        uint32_t v = (uint32_t)src[i];
        p[4 * i] = v >> 24; p[4 * i + 1] = v >> 16; p[4 * i + 2] = v >> 8; p[4 * i + 3] = v;
    }
}
void CopyFromEmuSwapped32(int* dst, uint32_t addr, size_t bytes)
{
    const uint8_t* p = PhysPtr(addr);
    for (size_t i = 0; i < bytes / 4; i++)
        dst[i] = (int)(((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3]);
}

// ---------------------------------------------------------------- accelerator
struct Accelerator
{
    uint32_t start = 0, end = 0, cur = 0;
    uint16_t format = 0;
    int16_t gain = 0, yn1 = 0, yn2 = 0;
    uint16_t predScale = 0;
    bool readsStopped = false;
    AXPBWii* pb = nullptr;

    void SetStart(uint32_t a) { start = a & 0x3FFFFFFF; }
    void SetEnd(uint32_t a) { end = a & 0x3FFFFFFF; }
    void SetCur(uint32_t a) { cur = a & 0xBFFFFFFF; }
    void SetYn2(int16_t v) { yn2 = v; readsStopped = false; }
    void SetPred(uint16_t v) { predScale = v & 0x7F; }

    uint16_t GetCurrentSample()
    {
        uint16_t val = 0;
        switch (format & 3)
        {
        case 0: val = ReadARAM(cur >> 1); val = (cur & 1) ? (val & 0xF) : (val >> 4); break;
        case 1: val = ReadARAM(cur); break;
        case 2: val = (uint16_t)((ReadARAM(cur * 2) << 8) | ReadARAM(cur * 2 + 1)); break;
        default: break;
        }
        return val;
    }

    void OnSampleReadEnd()
    {
        if (pb->audio_addr.looping)
        {
            SetPred(pb->adpcm_loop_info.pred_scale);
            if (pb->is_stream != 1)
            {
                yn1 = (int16_t)pb->adpcm_loop_info.yn1;
                SetYn2((int16_t)pb->adpcm_loop_info.yn2);
            }
            else
            {
                SetYn2(yn2);
            }
        }
        else
        {
            pb->running = 0;
        }
    }

    uint16_t ReadSample(const int16_t* coefs)
    {
        if (readsStopped) return 0;
        uint16_t val = 0;
        uint32_t stepSize = 0;
        uint32_t decode = (format >> 2) & 3;
        int16_t raw = (int16_t)GetCurrentSample();
        int coefIdx = (predScale >> 4) & 7;
        int32_t c1 = coefs[coefIdx * 2], c2 = coefs[coefIdx * 2 + 1];
        if (decode == 0)
        {
            raw &= 0xF;
            int scale = 1 << (predScale & 0xF);
            if (raw >= 8) raw -= 16;
            int32_t v32 = (scale * raw) + ((0x400 + c1 * yn1 + c2 * yn2) >> 11);
            val = (uint16_t)ClampS16(v32);
            stepSize = 2;
            yn2 = yn1;
            yn1 = (int16_t)val;
            cur += 1;
            if ((end & 0xF) == 0 && cur == end)
                cur = start + 1;
            else if ((end & 0xF) == 1 && cur == end - 1)
                cur = start;
            else if ((cur & 15) == 0)
            {
                predScale = ReadARAM((cur & ~15u) >> 1);
                cur += 2;
                stepSize += 2;
            }
        }
        else
        {
            int gs = 0;
            switch ((format >> 4) & 3) { case 0: gs = 11; break; case 1: gs = 0; break; case 2: gs = 16; break; }
            int32_t v32 = (((int32_t)gain * raw) >> gs) + ((c1 * yn1) >> gs) + ((c2 * yn2) >> gs);
            val = (uint16_t)(int16_t)v32;
            yn2 = yn1;
            yn1 = (int16_t)val;
            stepSize = 2;
            if (decode != 1) cur += 1;
        }
        if (cur == end + stepSize - 1)
        {
            cur = start;
            readsStopped = true;
            OnSampleReadEnd();
        }
        SetCur(cur);
        return val;
    }
};

// ---------------------------------------------------------------- state
int s_mainL[96], s_mainR[96], s_mainS[96];
int s_auxAL[96], s_auxAR[96], s_auxAS[96];
int s_auxBL[96], s_auxBR[96], s_auxBS[96];
int s_auxCL[96], s_auxCR[96], s_auxCS[96];
int s_wm[4][18], s_wmAux[4][18];
uint16_t s_lastMainVolume = 0x8000;
uint16_t s_lastAuxVolumes[3] = { 0x8000, 0x8000, 0x8000 };
uint32_t s_compressorPos = 0;
std::array<int16_t, 0x800> s_coeffs;
bool s_haveCoeffs = false;
uint32_t s_crc = 0;
int s_pbLayout = 2;     // 0 = full (with updates, hpf gap), 1 = no updates + hpf gap, 2 = no updates (biquad)
bool s_oldAXWii = false;
bool s_newFilter = true;
Accelerator s_acc;
uint16_t s_cmdlist[512];

void LoadCoeffs()
{
    static const char* paths[] = { "dsp_coef.bin", "C:\\dolphin_emu\\Dolphin-x64\\Sys\\GC\\dsp_coef.bin" };
    for (const char* p : paths)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, p, "rb") || !f) continue;
        uint8_t raw[0x1000];
        size_t n = fread(raw, 1, sizeof(raw), f);
        fclose(f);
        if (n != sizeof(raw)) continue;
        for (int i = 0; i < 0x800; i++) s_coeffs[i] = (int16_t)((raw[2 * i] << 8) | raw[2 * i + 1]);
        s_haveCoeffs = true;
        LOG_INFO("AX: using polyphase coefficients from {}", p);
        return;
    }
    LOG_WARN("AX: dsp_coef.bin not found; using linear resampling");
}

// ---------------------------------------------------------------- PB load/store
void ReadPB(uint32_t addr, AXPBWii& pb)
{
    char* dst = (char*)&pb;
    constexpr size_t ub = offsetof(AXPBWii, updates);
    constexpr size_t ue = ub + sizeof(PBUpdatesWii);
    constexpr size_t gb = offsetof(AXPBWii, hpf) + sizeof(PBHighPassFilter);
    constexpr size_t ge = offsetof(AXPBWii, biquad) + sizeof(PBBiquadFilter);
    memset(&pb, 0, sizeof(pb));
    switch (s_pbLayout)
    {
    case 0:
        CopyFromEmuSwapped16((uint16_t*)dst, addr, gb);
        CopyFromEmuSwapped16((uint16_t*)(dst + ge), addr + (uint32_t)gb, sizeof(pb) - ge);
        break;
    case 1:
        CopyFromEmuSwapped16((uint16_t*)dst, addr, ub);
        CopyFromEmuSwapped16((uint16_t*)(dst + ue), addr + (uint32_t)ub, gb - ue);
        CopyFromEmuSwapped16((uint16_t*)(dst + ge), addr + (uint32_t)gb, sizeof(pb) - ge);
        break;
    default:
        CopyFromEmuSwapped16((uint16_t*)dst, addr, ub);
        CopyFromEmuSwapped16((uint16_t*)(dst + ue), addr + (uint32_t)ub, sizeof(pb) - ue);
        break;
    }
}

void WritePB(uint32_t addr, const AXPBWii& pb)
{
    const char* src = (const char*)&pb;
    constexpr size_t ub = offsetof(AXPBWii, updates);
    constexpr size_t ue = ub + sizeof(PBUpdatesWii);
    constexpr size_t gb = offsetof(AXPBWii, hpf) + sizeof(PBHighPassFilter);
    constexpr size_t ge = offsetof(AXPBWii, biquad) + sizeof(PBBiquadFilter);
    switch (s_pbLayout)
    {
    case 0:
        CopyToEmuSwapped16(addr, (const uint16_t*)src, gb);
        CopyToEmuSwapped16(addr + (uint32_t)gb, (const uint16_t*)(src + ge), sizeof(pb) - ge);
        break;
    case 1:
        CopyToEmuSwapped16(addr, (const uint16_t*)src, ub);
        CopyToEmuSwapped16(addr + (uint32_t)ub, (const uint16_t*)(src + ue), gb - ue);
        CopyToEmuSwapped16(addr + (uint32_t)gb, (const uint16_t*)(src + ge), sizeof(pb) - ge);
        break;
    default:
        CopyToEmuSwapped16(addr, (const uint16_t*)src, ub);
        CopyToEmuSwapped16(addr + (uint32_t)ub, (const uint16_t*)(src + ue), sizeof(pb) - ue);
        break;
    }
}

// ---------------------------------------------------------------- voice processing
uint32_t Resample(const std::function<int16_t(uint32_t)>& input, int16_t* output, uint32_t count, int16_t* last, uint32_t pos, uint32_t ratio, int srctype, const int16_t* coeffs)
{
    int readCount = 0;
    if (coeffs && srctype == SRCTYPE_POLYPHASE)
    {
        int16_t temp[4];
        uint32_t idx = 0;
        temp[idx++ & 3] = last[0]; temp[idx++ & 3] = last[1]; temp[idx++ & 3] = last[2]; temp[idx++ & 3] = last[3];
        for (uint32_t i = 0; i < count; ++i)
        {
            pos += ratio;
            while (pos >= 0x10000)
            {
                temp[idx++ & 3] = input(readCount++);
                pos -= 0x10000;
            }
            uint16_t frac = ((pos & 0xFFFF) >> 9) << 2;
            const int16_t* c = &coeffs[frac];
            int64_t t0 = temp[idx++ & 3], t1 = temp[idx++ & 3], t2 = temp[idx++ & 3], t3 = temp[idx++ & 3];
            int64_t s = (t0 * c[0] + t1 * c[1] + t2 * c[2] + t3 * c[3]) >> 15;
            output[i] = ClampS16(s);
        }
        last[3] = temp[--idx & 3]; last[2] = temp[--idx & 3]; last[1] = temp[--idx & 3]; last[0] = temp[--idx & 3];
    }
    else if (srctype == SRCTYPE_LINEAR || srctype == SRCTYPE_POLYPHASE)
    {
        int16_t temp[4];
        uint32_t idx = 0;
        temp[idx++ & 3] = last[0]; temp[idx++ & 3] = last[1]; temp[idx++ & 3] = last[2]; temp[idx++ & 3] = last[3];
        for (uint32_t i = 0; i < count; ++i)
        {
            pos += ratio;
            while (pos >= 0x10000)
            {
                temp[idx++ & 3] = input(readCount++);
                pos -= 0x10000;
            }
            uint16_t frac = pos & 0xFFFF;
            uint16_t inv = (uint16_t)-(int)frac;
            int16_t s;
            if (frac)
            {
                int32_t s0 = temp[idx++ & 3], s1 = temp[idx++ & 3];
                s = (int16_t)(((s0 * inv) + (s1 * frac)) >> 16);
                idx += 2;
            }
            else
            {
                s = temp[idx++ & 3];
                idx += 3;
            }
            output[i] = s;
        }
        last[3] = temp[--idx & 3]; last[2] = temp[--idx & 3]; last[1] = temp[--idx & 3]; last[0] = temp[--idx & 3];
    }
    else
    {
        for (uint32_t i = 0; i < count; ++i) output[i] = input(i);
        memcpy(last, output + count - 4, 4 * sizeof(int16_t));
    }
    return pos;
}

void MixAdd(int* out, const int16_t* in, uint32_t count, VolumeData* vd, int16_t* dpop, bool ramp)
{
    uint16_t& volume = vd->volume;
    uint16_t delta = ramp ? vd->volume_delta : 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        int64_t s = in[i];
        s *= volume;
        s >>= 15;
        int16_t s16 = ClampS16((int32_t)s);
        out[i] += s16;
        volume += delta;
        *dpop = s16;
    }
}

void LowPass(int16_t* s, uint32_t count, PBLowPassFilter& f)
{
    for (uint32_t i = 0; i < count; ++i)
        f.yn1 = s[i] = ClampS16((f.a0 * (int32_t)s[i] + f.b0 * (int32_t)f.yn1) >> 15);
}

void Biquad(int16_t* s, uint32_t count, PBBiquadFilter& f)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        int16_t xn0 = s[i];
        int64_t t = 0;
        t += f.b0 * (int32_t)xn0;
        t += f.b1 * (int32_t)f.xn1;
        t += f.b2 * (int32_t)f.xn2;
        t += f.a1 * (int32_t)f.yn1;
        t += f.a2 * (int32_t)f.yn2;
        t <<= 2;
        if (t & 0x10000) t += 0x8000;
        else t += 0x7FFF;
        t >>= 16;
        int16_t yn0 = ClampS16(t);
        f.xn2 = f.xn1; f.yn2 = f.yn1; f.xn1 = xn0; f.yn1 = yn0;
        s[i] = yn0;
    }
}

uint32_t ConvertMixerControl(uint32_t mc)
{
    uint32_t r = 0;
    if (mc & 0x00000001) r |= MIX_MAIN_L;
    if (mc & 0x00000002) r |= MIX_MAIN_R;
    if (mc & 0x00000004) r |= MIX_MAIN_L | MIX_MAIN_R | MIX_MAIN_L_RAMP | MIX_MAIN_R_RAMP;
    if (mc & 0x00000008) r |= MIX_MAIN_S;
    if (mc & 0x00000010) r |= MIX_MAIN_S | MIX_MAIN_S_RAMP;
    if (mc & 0x00010000) r |= MIX_AUXA_L;
    if (mc & 0x00020000) r |= MIX_AUXA_R;
    if (mc & 0x00040000) r |= MIX_AUXA_L | MIX_AUXA_R | MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP;
    if (mc & 0x00080000) r |= MIX_AUXA_S;
    if (mc & 0x00100000) r |= MIX_AUXA_S | MIX_AUXA_S_RAMP;
    if (mc & 0x00200000) r |= MIX_AUXB_L;
    if (mc & 0x00400000) r |= MIX_AUXB_R;
    if (mc & 0x00800000) r |= MIX_AUXB_L | MIX_AUXB_R | MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP;
    if (mc & 0x01000000) r |= MIX_AUXB_S;
    if (mc & 0x02000000) r |= MIX_AUXB_S | MIX_AUXB_S_RAMP;
    if (mc & 0x04000000) r |= MIX_AUXC_L;
    if (mc & 0x08000000) r |= MIX_AUXC_R;
    if (mc & 0x10000000) r |= MIX_AUXC_L | MIX_AUXC_R | MIX_AUXC_L_RAMP | MIX_AUXC_R_RAMP;
    if (mc & 0x20000000) r |= MIX_AUXC_S;
    if (mc & 0x40000000) r |= MIX_AUXC_S | MIX_AUXC_S_RAMP;
    return r;
}

struct Buffers
{
    int* regular[12];   // mainL mainR mainS, auxA LRS, auxB LRS, auxC LRS
    int* wm[8];         // main0 aux0 main1 aux1 main2 aux2 main3 aux3
};

void ProcessVoice(AXPBWii& pb, const Buffers& b, uint16_t count, uint32_t mctrl, const int16_t* coeffs)
{
    if (pb.running != 1) return;
    int16_t samples[96];

    // input + SRC
    Accelerator& acc = s_acc;
    acc.pb = &pb;
    acc.SetStart(HILO(pb.audio_addr.loop_addr_hi, pb.audio_addr.loop_addr_lo));
    acc.SetEnd(HILO(pb.audio_addr.end_addr_hi, pb.audio_addr.end_addr_lo));
    acc.SetCur(HILO(pb.audio_addr.cur_addr_hi, pb.audio_addr.cur_addr_lo));
    acc.format = pb.audio_addr.sample_format;
    acc.yn1 = pb.adpcm.yn1;
    acc.SetYn2(pb.adpcm.yn2);
    acc.gain = (int16_t)pb.adpcm.gain;
    acc.SetPred(pb.adpcm.pred_scale);
    const int16_t* c = coeffs ? coeffs + pb.coef_select * 0x200 : nullptr;
    uint32_t pos = Resample([&](uint32_t) { return (int16_t)acc.ReadSample(pb.adpcm.coefs); }, samples, count, pb.src.last_samples,
                            pb.src.cur_addr_frac, HILO(pb.src.ratio_hi, pb.src.ratio_lo), pb.src_type, c);
    pb.src.cur_addr_frac = (uint16_t)(pos & 0xFFFF);
    pb.audio_addr.cur_addr_hi = (uint16_t)(acc.cur >> 16);
    pb.audio_addr.cur_addr_lo = (uint16_t)acc.cur;
    pb.adpcm.yn1 = acc.yn1;
    pb.adpcm.yn2 = acc.yn2;
    pb.adpcm.pred_scale = acc.predScale;

    for (uint32_t i = 0; i < count; ++i)
    {
        int32_t vol = (uint16_t)pb.vol_env.cur_volume;
        samples[i] = ClampS16(((int32_t)samples[i] * vol) >> 15);
        pb.vol_env.cur_volume += pb.vol_env.cur_volume_delta;
    }
    if (pb.lpf.on) LowPass(samples, count, pb.lpf);
    if (s_newFilter && pb.biquad.on) Biquad(samples, count, pb.biquad);

#define MIXON(f, buf, vol, dp, ramp) if (mctrl & (f)) MixAdd(buf, samples, count, &pb.mixer.vol, &pb.dpop.dp, (mctrl & (ramp)) != 0)
    MIXON(MIX_MAIN_L, b.regular[0], main_left, main_left, MIX_MAIN_L_RAMP);
    MIXON(MIX_MAIN_R, b.regular[1], main_right, main_right, MIX_MAIN_R_RAMP);
    MIXON(MIX_MAIN_S, b.regular[2], main_surround, main_surround, MIX_MAIN_S_RAMP);
    MIXON(MIX_AUXA_L, b.regular[3], auxA_left, auxA_left, MIX_AUXA_L_RAMP);
    MIXON(MIX_AUXA_R, b.regular[4], auxA_right, auxA_right, MIX_AUXA_R_RAMP);
    MIXON(MIX_AUXA_S, b.regular[5], auxA_surround, auxA_surround, MIX_AUXA_S_RAMP);
    MIXON(MIX_AUXB_L, b.regular[6], auxB_left, auxB_left, MIX_AUXB_L_RAMP);
    MIXON(MIX_AUXB_R, b.regular[7], auxB_right, auxB_right, MIX_AUXB_R_RAMP);
    MIXON(MIX_AUXB_S, b.regular[8], auxB_surround, auxB_surround, MIX_AUXB_S_RAMP);
    MIXON(MIX_AUXC_L, b.regular[9], auxC_left, auxC_left, MIX_AUXC_L_RAMP);
    MIXON(MIX_AUXC_R, b.regular[10], auxC_right, auxC_right, MIX_AUXC_R_RAMP);
    MIXON(MIX_AUXC_S, b.regular[11], auxC_surround, auxC_surround, MIX_AUXC_S_RAMP);
#undef MIXON

    if (pb.remote)
    {
        if (s_newFilter && pb.remote_iir.on)
        {
            if (pb.remote_iir.on == 2) Biquad(samples, count, pb.remote_iir.biquad);
            else LowPass(samples, count, pb.remote_iir.lpf);
        }
        uint16_t wmCount = count == 96 ? 18 : 6;
        int16_t wmSamples[18];
        uint32_t p2 = Resample([&](uint32_t i) { return samples[i]; }, wmSamples, wmCount, pb.remote_src.last_samples,
                               pb.remote_src.cur_addr_frac, 0x55555, SRCTYPE_POLYPHASE, coeffs);
        pb.remote_src.cur_addr_frac = (uint16_t)(p2 & 0xFFFF);
        VolumeData* vols[8] = { &pb.remote_mixer.main0, &pb.remote_mixer.aux0, &pb.remote_mixer.main1, &pb.remote_mixer.aux1,
                                &pb.remote_mixer.main2, &pb.remote_mixer.aux2, &pb.remote_mixer.main3, &pb.remote_mixer.aux3 };
        int16_t* dps[8] = { &pb.remote_dpop.main0, &pb.remote_dpop.aux0, &pb.remote_dpop.main1, &pb.remote_dpop.aux1,
                            &pb.remote_dpop.main2, &pb.remote_dpop.aux2, &pb.remote_dpop.main3, &pb.remote_dpop.aux3 };
        for (int n = 0; n < 8; n++)
            if ((pb.remote_mixer_control >> (2 * n)) & 3)
                MixAdd(b.wm[n], wmSamples, wmCount, vols[n], dps[n], ((pb.remote_mixer_control >> (2 * n)) & 2) != 0);
    }
}

Buffers MakeBuffers()
{
    Buffers b{ { s_mainL, s_mainR, s_mainS, s_auxAL, s_auxAR, s_auxAS, s_auxBL, s_auxBR, s_auxBS, s_auxCL, s_auxCR, s_auxCS },
               { s_wm[0], s_wmAux[0], s_wm[1], s_wmAux[1], s_wm[2], s_wmAux[2], s_wm[3], s_wmAux[3] } };
    return b;
}

void ProcessPBList(uint32_t pbAddr)
{
    AXPBWii pb;
    const int16_t* coeffs = s_haveCoeffs ? s_coeffs.data() : nullptr;
    int guard = 0;
    while (pbAddr && guard++ < 1024)
    {
        Buffers b = MakeBuffers();
        ReadPB(pbAddr, pb);
        uint32_t mc = ConvertMixerControl(HILO(pb.mixer_control_hi, pb.mixer_control_lo));
        if (s_oldAXWii && (pb.updates.num_updates[0] | pb.updates.num_updates[1] | pb.updates.num_updates[2]))
        {
            uint16_t upd[64];
            CopyFromEmuSwapped16(upd, HILO(pb.updates.data_hi, pb.updates.data_lo), sizeof(upd));
            uint32_t startIdx = 0;
            for (int ms = 0; ms < 3; ++ms)
            {
                for (uint32_t i = startIdx; i < startIdx + pb.updates.num_updates[ms] && i < 32; i++)
                {
                    uint16_t off = upd[2 * i], val = upd[2 * i + 1];
                    if (off < sizeof(pb) / 2) ((uint16_t*)&pb)[off] = val;
                }
                startIdx += pb.updates.num_updates[ms];
                ProcessVoice(pb, b, 32, mc, coeffs);
                for (auto& p : b.regular) p += 32;
                for (auto& p : b.wm) p += 6;
            }
        }
        else
        {
            ProcessVoice(pb, b, 96, mc, coeffs);
        }
        WritePB(pbAddr, pb);
        pbAddr = HILO(pb.next_pb_hi, pb.next_pb_lo);
    }
}

void GenerateVolumeRamp(uint16_t* out, uint16_t v1, uint16_t v2, size_t n)
{
    float cur = v1;
    for (size_t i = 0; i < n; ++i)
    {
        cur += (v2 - v1) / (float)n;
        out[i] = (uint16_t)cur;
    }
}

void SetupProcessing(uint32_t initAddr)
{
    struct BD { int* p; int spm; };
    BD bufs[20] = {
        { s_mainL, 32 }, { s_mainR, 32 }, { s_mainS, 32 }, { s_auxAL, 32 }, { s_auxAR, 32 }, { s_auxAS, 32 },
        { s_auxBL, 32 }, { s_auxBR, 32 }, { s_auxBS, 32 }, { s_auxCL, 32 }, { s_auxCR, 32 }, { s_auxCS, 32 },
        { s_wm[0], 6 }, { s_wmAux[0], 6 }, { s_wm[1], 6 }, { s_wmAux[1], 6 }, { s_wm[2], 6 }, { s_wmAux[2], 6 }, { s_wm[3], 6 }, { s_wmAux[3], 6 },
    };
    uint16_t init[60];
    CopyFromEmuSwapped16(init, initAddr, sizeof(init));
    for (int i = 0; i < 20; i++)
    {
        int32_t value = (int32_t)(((uint32_t)init[3 * i] << 16) | init[3 * i + 1]);
        int16_t delta = (int16_t)init[3 * i + 2];
        int n = 3 * bufs[i].spm;
        if (value == 0) memset(bufs[i].p, 0, n * sizeof(int));
        else for (int j = 0; j < n; ++j) bufs[i].p[j] = value + j * delta;
    }
}

void AddToLR(uint32_t addr, bool neg)
{
    int v[96];
    CopyFromEmuSwapped32(v, addr, sizeof(v));
    for (int i = 0; i < 96; ++i)
    {
        int val = neg ? -v[i] : v[i];
        s_mainL[i] += val;
        s_mainR[i] += val;
    }
}

void AddSubToLR(uint32_t addr)
{
    int v[192];
    CopyFromEmuSwapped32(v, addr, sizeof(v));
    for (int i = 0; i < 96; ++i) s_mainL[i] += v[i];
    for (int i = 0; i < 96; ++i) s_mainR[i] -= v[96 + i];
}

void MixAUXSamples(int auxId, uint32_t writeAddr, uint32_t readAddr, uint16_t volume)
{
    uint16_t ramp[96];
    GenerateVolumeRamp(ramp, s_lastAuxVolumes[auxId], volume, 96);
    s_lastAuxVolumes[auxId] = volume;
    int* mains[3] = { s_mainL, s_mainR, s_mainS };
    int* aux[3];
    switch (auxId)
    {
    case 0: aux[0] = s_auxAL; aux[1] = s_auxAR; aux[2] = s_auxAS; break;
    case 1: aux[0] = s_auxBL; aux[1] = s_auxBR; aux[2] = s_auxBS; break;
    default: aux[0] = s_auxCL; aux[1] = s_auxCR; aux[2] = s_auxCS; break;
    }
    if (writeAddr)
    {
        for (auto* b : aux)
        {
            CopyToEmuSwapped32(writeAddr, b, 96 * sizeof(int));
            writeAddr += 96 * sizeof(int);
        }
    }
    int in[3 * 96];
    CopyFromEmuSwapped32(in, readAddr, sizeof(in));
    for (int m = 0; m < 3; m++)
        for (int j = 0; j < 96; ++j)
        {
            int64_t s = (int64_t)in[m * 96 + j];
            s *= ramp[j];
            mains[m][j] += (int32_t)(s >> 15);
        }
}

void UploadAUXMixLRSC(int auxId, const uint32_t* a, uint16_t volume)
{
    int* l = auxId ? s_auxBL : s_auxAL;
    int* r = auxId ? s_auxBR : s_auxAR;
    int* s = auxId ? s_auxBS : s_auxAS;
    int* c = auxId ? s_auxCS : s_auxCR;
    CopyToEmuSwapped32(a[0], l, 96 * sizeof(int));
    CopyToEmuSwapped32(a[0] + 96 * sizeof(int), r, 96 * sizeof(int));
    CopyToEmuSwapped32(a[0] + 2 * 96 * sizeof(int), s, 96 * sizeof(int));
    CopyToEmuSwapped32(a[1], c, 96 * sizeof(int));
    uint16_t ramp[96];
    GenerateVolumeRamp(ramp, s_lastAuxVolumes[auxId], volume, 96);
    s_lastAuxVolumes[auxId] = volume;
    int* dest[4] = { s_mainL, s_mainR, s_mainS, s_auxCL };
    for (int m = 0; m < 4; ++m)
    {
        CopyFromEmuSwapped32(l, a[2 + m], 96 * sizeof(int));
        for (int i = 0; i < 96; ++i)
        {
            int64_t smp = (int64_t)l[i];
            smp *= ramp[i];
            dest[m][i] += (int32_t)(smp >> 15);
        }
    }
}

void RunCompressor(uint16_t threshold, uint16_t releaseFrames, uint32_t table, uint32_t millis)
{
    bool triggered = false;
    for (uint32_t i = 0; i < 32 * millis; ++i)
        if (std::abs(s_mainL[i]) > (int)threshold || std::abs(s_mainR[i]) > (int)threshold) { triggered = true; break; }
    uint32_t frameBytes = 32 * millis * 2;
    uint32_t off;
    if (triggered)
    {
        off = s_compressorPos * frameBytes;
        s_compressorPos = releaseFrames;
    }
    else if (s_compressorPos)
    {
        --s_compressorPos;
        off = (11 + s_compressorPos) * frameBytes;
    }
    else
        return;
    uint16_t rampv[96];
    CopyFromEmuSwapped16(rampv, table + off, 32 * millis * 2);
    for (uint32_t i = 0; i < 32 * millis; ++i)
    {
        s_mainL[i] = (int)(((int64_t)s_mainL[i] * rampv[i]) >> 15);
        s_mainR[i] = (int)(((int64_t)s_mainR[i] * rampv[i]) >> 15);
    }
}

void OutputSamples(uint32_t lrAddr, uint32_t surroundAddr, uint16_t volume, bool uploadAuxC)
{
    uint16_t ramp[96];
    GenerateVolumeRamp(ramp, s_lastMainVolume, volume, 96);
    s_lastMainVolume = volume;
    CopyToEmuSwapped32(surroundAddr, s_mainS, 96 * sizeof(int));
    if (uploadAuxC) CopyToEmuSwapped32(surroundAddr + 96 * sizeof(int), s_auxCL, 96 * sizeof(int));
    for (int i = 0; i < 96; ++i)
    {
        int64_t l = ((int64_t)s_mainL[i] * ramp[i]) >> 15;
        int64_t r = ((int64_t)s_mainR[i] * ramp[i]) >> 15;
        s_mainL[i] = ClampS16(l);
        s_mainR[i] = ClampS16(r);
    }
    uint16_t buf[192];
    for (int i = 0; i < 96; ++i)
    {
        buf[2 * i] = (uint16_t)(int16_t)s_mainR[i];
        buf[2 * i + 1] = (uint16_t)(int16_t)s_mainL[i];
    }
    CopyToEmuSwapped16(lrAddr, buf, sizeof(buf));
    DSP_PushMail(0xDCD10004, true, 0);   // DSP_SYNC
}

void OutputWMSamples(const uint32_t* a)
{
    for (int i = 0; i < 4; ++i)
    {
        uint16_t out[18];
        for (int j = 0; j < 18; ++j) out[j] = (uint16_t)ClampS16(s_wm[i][j]);
        CopyToEmuSwapped16(a[i], out, sizeof(out));
    }
}
}   // namespace

void AX_Reset()
{
    s_lastMainVolume = 0x8000;
    for (auto& v : s_lastAuxVolumes) v = 0x8000;
    s_compressorPos = 0;
    static bool loaded = false;
    if (!loaded)
    {
        LoadCoeffs();
        loaded = true;
    }
}

void AX_SetUCodeCRC(uint32_t crc)
{
    s_crc = crc;
    s_oldAXWii = crc == 0xfa450138 || crc == 0x7699af32;
    s_newFilter = crc == 0x347112ba || crc == 0x4cc52064;
    if (crc == 0x7699af32 || crc == 0xfa450138) s_pbLayout = 0;
    else if (crc == 0xd9c4bf34 || crc == 0xadbc06bd) s_pbLayout = 1;
    else s_pbLayout = 2;
    LOG_INFO("AX: ucode crc {:08x} -> layout {} old={} newFilter={}", crc, s_pbLayout, s_oldAXWii, s_newFilter);
}

void AX_ProcessCommandList(uint32_t addr, uint16_t size)
{
    if (size >= 512) { LOG_ERROR("AX: command list too large ({})", size); return; }
    CopyFromEmuSwapped16(s_cmdlist, addr, size * 2);
    uint32_t i = 0;
    auto nxt = [&]() -> uint16_t { return i < 512 ? s_cmdlist[i++] : 0; };
    auto nxt32 = [&]() -> uint32_t { uint32_t hi = nxt(); return (hi << 16) | nxt(); };
    uint32_t pbAddr = 0;
    for (int guard = 0; guard < 256; guard++)
    {
        uint16_t cmd = nxt();
        if (s_oldAXWii)
        {
            switch (cmd)
            {
            case 0x00: SetupProcessing(nxt32()); break;
            case 0x01: case 0x02: AddToLR(nxt32(), cmd == 0x02); break;
            case 0x03: AddSubToLR(nxt32()); break;
            case 0x04: pbAddr = nxt32(); break;
            case 0x05: ProcessPBList(pbAddr); break;
            case 0x06: case 0x07: case 0x08: { uint16_t vol = nxt(); uint32_t a = nxt32(); uint32_t b = nxt32(); MixAUXSamples(cmd - 0x06, a, b, vol); break; }
            case 0x09: case 0x0A: { uint16_t vol = nxt(); uint32_t a[6]; for (auto& x : a) x = nxt32(); UploadAUXMixLRSC(cmd == 0x0A, a, vol); break; }
            case 0x0B: { uint16_t th = nxt(), fr = nxt(); RunCompressor(th, fr, nxt32(), 3); break; }
            case 0x0C: case 0x0D: { uint32_t a = nxt32(); uint32_t b = nxt32(); OutputSamples(b, a, 0x8000, cmd == 0x0D); break; }
            case 0x0E: { uint32_t a[4]; for (auto& x : a) x = nxt32(); OutputWMSamples(a); break; }
            case 0x0F: return;
            default: LOG_WARN("AX(old): unknown command {:x}", cmd); return;
            }
        }
        else
        {
            switch (cmd)
            {
            case 0x00: SetupProcessing(nxt32()); break;
            case 0x01: case 0x02: AddToLR(nxt32(), cmd == 0x02); break;
            case 0x03: AddSubToLR(nxt32()); break;
            case 0x04: ProcessPBList(nxt32()); break;
            case 0x05: case 0x06: case 0x07: { uint16_t vol = nxt(); uint32_t a = nxt32(); uint32_t b = nxt32(); MixAUXSamples(cmd - 0x05, a, b, vol); break; }
            case 0x08: case 0x09: { uint16_t vol = nxt(); uint32_t a[6]; for (auto& x : a) x = nxt32(); UploadAUXMixLRSC(cmd == 0x09, a, vol); break; }
            case 0x0A: { uint16_t th = nxt(), fr = nxt(); RunCompressor(th, fr, nxt32(), 3); break; }
            case 0x0B: case 0x0C:
            {
                uint16_t vol = s_crc == 0xd9c4bf34 ? 0x8000 : nxt();
                uint32_t a = nxt32();
                uint32_t b = nxt32();
                OutputSamples(b, a, vol, cmd == 0x0C);
                break;
            }
            case 0x0D: { uint32_t a[4]; for (auto& x : a) x = nxt32(); OutputWMSamples(a); break; }
            case 0x0E: return;
            default: LOG_WARN("AX: unknown command {:x}", cmd); return;
            }
        }
    }
}
