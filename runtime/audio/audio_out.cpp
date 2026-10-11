// Audio output through XAudio2.
#include "audio_out.h"
#include "../log.h"

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
int g_audioPeak = 0;   // max |sample| since last status line

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
    LOG_INFO("XAudio2 initialised");
}

void AudioOut_PushDSP(const uint8_t* data, uint32_t bytes, uint32_t sampleRate)
{
    if (!s_xa) return;
    if (!s_voice || s_rate != sampleRate) CreateVoice(sampleRate);
    if (!s_voice) return;
    uint32_t frames = bytes / 4;
    // DMA data is big-endian, right channel first
    for (uint32_t i = 0; i < frames; i++)
    {
        int16_t r = (int16_t)((data[4 * i] << 8) | data[4 * i + 1]);
        int16_t l = (int16_t)((data[4 * i + 2] << 8) | data[4 * i + 3]);
        g_audioPeak = std::max(g_audioPeak, std::max(std::abs((int)l), std::abs((int)r)));
        s_staging.push_back((int16_t)(l * g_audioVolume));
        s_staging.push_back((int16_t)(r * g_audioVolume));
    }
    uint32_t batch = sampleRate / BATCH_FRAMES_PER_SEC_DIV;
    if (s_staging.size() / 2 < batch) return;

    XAUDIO2_VOICE_STATE st;
    s_voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    while (s_inFlight.size() > st.BuffersQueued) s_inFlight.pop_front();
    if (st.BuffersQueued > 8)
    {
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
