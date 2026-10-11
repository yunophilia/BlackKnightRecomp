#pragma once
#include "ppc_context.h"

constexpr uint32_t MSR_EE = 0x8000;
constexpr uint64_t TB_FREQ = 243000000ull / 4;   // 60.75 MHz

void Cpu_Init();
void Cpu_SetFunction(uint32_t guest, PPCFunc f);

void Timebase_Init();
uint64_t Timebase_Now();
void Timebase_Set(uint64_t v);

uint32_t Dec_Read();
void Dec_Write(uint32_t v);
bool Dec_Expired();
void Dec_Ack();
uint64_t Dec_Deadline(bool* armed);

inline uint64_t TbFromMs(double ms) { return (uint64_t)(ms * (TB_FREQ / 1000.0)); }
