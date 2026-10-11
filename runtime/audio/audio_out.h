#pragma once
#include <cstdint>

void AudioOut_Init();
// Big-endian interleaved stereo s16 samples as DMA'd by the AI.
void AudioOut_PushDSP(const uint8_t* data, uint32_t bytes, uint32_t sampleRate);
