#pragma once
#include <cstdint>

void AudioOut_Init();
// Big-endian interleaved stereo s16 samples as DMA'd by the AI.
void AudioOut_PushDSP(const uint8_t* data, uint32_t bytes, uint32_t sampleRate);
// Mono s16 PCM decoded from a Wii Remote speaker stream (one voice per remote).
void AudioOut_PushSpeaker(int chan, const int16_t* pcm, uint32_t samples);
