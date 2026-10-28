// Audio output through XAudio2.
#include "../../audio/audio_out.h"
#include "../../log.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <vector>
#include <windows.h>
#include <xaudio2.h>

static IXAudio2* s_xa = nullptr;
static IXAudio2MasteringVoice* s_master = nullptr;
static IXAudio2SourceVoice* s_voice = nullptr;
static uint32_t s_rate = 0;
static std::vector<int16_t> s_staging;
static std::deque<std::vector<int16_t>> s_inFlight;   // buffers owned until played
static constexpr uint32_t BATCH_FRAMES_PER_SEC_DIV = 100;   // 10 ms batches
float g_audioVolume = 1.0f;
int g_audioPeak = 0;
int g_audioUnderruns = 0, g_audioDrops = 0;   // since last status line
uint64_t g_audioFrames = 0;
int g_audioRepeats = 0;                        // AI blocks identical to the previous one                    // stereo frames produced (total)   // max |sample| since last status line
FILE* g_audioDump = nullptr;   // --dump-audio: raw interleaved s16 stereo (host order)

static void CreateVoice(uint32_t rate)
{
    if (s_voice)
    {
        s_voice->DestroyVoice();
        s_voice = nullptr;
        s_inFlight.clear();
    }
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = rate * 4;
    if (FAILED(s_xa->CreateSourceVoice(&s_voice, &wf)))
    {
        LOG_ERROR("XAudio2: CreateSourceVoice failed");
        s_voice = nullptr;
        return;
    }
    s_voice->Start();
    s_rate = rate;
}

bool g_audioMute = false;   // --mute (test runs)

void AudioOut_Init()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(XAudio2Create(&s_xa, 0, XAUDIO2_DEFAULT_PROCESSOR)))
    {
        LOG_ERROR("XAudio2Create failed; audio disabled");
        return;
    }
    if (FAILED(s_xa->CreateMasteringVoice(&s_master)))
    {
        LOG_ERROR("XAudio2: no mastering voice; audio disabled");
        s_xa->Release();
        s_xa = nullptr;
        return;
    }
    extern bool g_audioMute;
    if (g_audioMute) s_master->SetVolume(0.0f);
    LOG_INFO("XAudio2 initialised{}", g_audioMute ? " (muted)" : "");
}

void AudioOut_PushDSP(const uint8_t* data, uint32_t bytes, uint32_t sampleRate)
{
    if (!s_xa) return;
    if (!s_voice || s_rate != sampleRate) CreateVoice(sampleRate);
    if (!s_voice) return;
    uint32_t frames = bytes / 4;
    g_audioFrames += frames;
    // DMA data is big-endian, right channel first
    for (uint32_t i = 0; i < frames; i++)
    {
        int16_t r = (int16_t)((data[4 * i] << 8) | data[4 * i + 1]);
        int16_t l = (int16_t)((data[4 * i + 2] << 8) | data[4 * i + 3]);
        g_audioPeak = std::max(g_audioPeak, std::max(std::abs((int)l), std::abs((int)r)));
        s_staging.push_back((int16_t)(l * g_audioVolume));
        s_staging.push_back((int16_t)(r * g_audioVolume));
        if (g_audioDump) { int16_t lr[2] = { l, r }; fwrite(lr, 2, 2, g_audioDump); }
    }
    uint32_t batch = sampleRate / BATCH_FRAMES_PER_SEC_DIV;
    if (s_staging.size() / 2 < batch) return;

    XAUDIO2_VOICE_STATE st;
    s_voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    while (s_inFlight.size() > st.BuffersQueued) s_inFlight.pop_front();
    if (st.BuffersQueued == 0)
    {
        // ran dry: rebuild a 30 ms cushion before resuming so jitter is absorbed
        if (s_staging.size() / 2 < batch * 3) return;
        g_audioUnderruns++;
    }
    if (st.BuffersQueued > 8)
    {
        g_audioDrops++;
        // too much latency: drop this batch
        s_staging.clear();
        return;
    }
    s_inFlight.push_back(std::move(s_staging));
    s_staging.clear();
    auto& buf = s_inFlight.back();
    XAUDIO2_BUFFER xb{};
    xb.AudioBytes = (UINT32)(buf.size() * sizeof(int16_t));
    xb.pAudioData = (const BYTE*)buf.data();
    s_voice->SubmitSourceBuffer(&xb);
}

// ---------------------------------------------------------------- Wii Remote speakers
// The SDK configures the speaker for 4-bit ADPCM at 12 MHz / 2000 = 6000 Hz.
static constexpr uint32_t SPEAKER_RATE = 6000;
float g_speakerVolume = 0.6f;   // the remote speaker is quieter than the TV
static IXAudio2SourceVoice* s_spkVoice[4];
static std::vector<int16_t> s_spkStaging[4];
static std::deque<std::vector<int16_t>> s_spkInFlight[4];

void AudioOut_PushSpeaker(int chan, const int16_t* pcm, uint32_t samples)
{
    if (!s_xa || chan < 0 || chan > 3) return;
    if (!s_spkVoice[chan])
    {
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 1;
        wf.nSamplesPerSec = SPEAKER_RATE;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = 2;
        wf.nAvgBytesPerSec = SPEAKER_RATE * 2;
        if (FAILED(s_xa->CreateSourceVoice(&s_spkVoice[chan], &wf))) return;
        s_spkVoice[chan]->Start();
    }
    if (g_audioDump)
    {
        static FILE* f = fopen("speaker_dump.raw", "wb");
        if (f) { fwrite(pcm, 2, samples, f); fflush(f); }
    }
    auto& st = s_spkStaging[chan];
    for (uint32_t i = 0; i < samples; i++) st.push_back((int16_t)(pcm[i] * g_speakerVolume * g_audioVolume));
    if (st.size() < SPEAKER_RATE / 50) return;   // 20 ms batches
    XAUDIO2_VOICE_STATE vs;
    s_spkVoice[chan]->GetState(&vs, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    auto& fl = s_spkInFlight[chan];
    while (fl.size() > vs.BuffersQueued) fl.pop_front();
    if (vs.BuffersQueued > 6)
    {
        st.clear();
        return;
    }
    fl.push_back(std::move(st));
    st.clear();
    XAUDIO2_BUFFER xb{};
    xb.AudioBytes = (UINT32)(fl.back().size() * sizeof(int16_t));
    xb.pAudioData = (const BYTE*)fl.back().data();
    s_spkVoice[chan]->SubmitSourceBuffer(&xb);
}
