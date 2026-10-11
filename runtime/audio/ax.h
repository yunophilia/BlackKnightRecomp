#pragma once
#include <cstdint>

// HLE of the Wii AX DSP microcode.
void AX_Reset();
void AX_SetUCodeCRC(uint32_t crc);
void AX_ProcessCommandList(uint32_t addr, uint16_t sizeWords);
