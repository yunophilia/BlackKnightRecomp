#pragma once
#include <cstdint>

// Polyphase resampling coefficients of the DSP ROM (0x800 big-endian words in a real
// dump). Generates Dolphin's free replacement table (Data/Sys/GC/dsp_coef.bin),
// bit for bit, from the formula in Dolphin's docs/DSP/free_dsp_rom/generate_coefs.py.
void DSP_GenerateCoefs(int16_t out[0x800]);
