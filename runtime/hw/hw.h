#pragma once
#include <cstdint>

// PI interrupt causes
enum : uint32_t
{
    INT_CAUSE_PI = 0x1,
    INT_CAUSE_RSW = 0x2,
    INT_CAUSE_DI = 0x4,
    INT_CAUSE_SI = 0x8,
    INT_CAUSE_EXI = 0x10,
    INT_CAUSE_AI = 0x20,
    INT_CAUSE_DSP = 0x40,
    INT_CAUSE_MEMORY = 0x80,
    INT_CAUSE_VI = 0x100,
    INT_CAUSE_PE_TOKEN = 0x200,
    INT_CAUSE_PE_FINISH = 0x400,
    INT_CAUSE_CP = 0x800,
    INT_CAUSE_DEBUG = 0x1000,
    INT_CAUSE_HSP = 0x2000,
    INT_CAUSE_WII_IPC = 0x4000,
    INT_CAUSE_RST_BUTTON = 0x10000,
};

void HW_Init();
bool HW_PIInterruptPending();
void PI_SetInterrupt(uint32_t cause, bool set);

// Per-block register handlers. 16-bit blocks get read16/write16, 32-bit blocks
// read32/write32; the dispatcher splits/merges accesses as needed.
uint16_t CP_Read16(uint32_t off);
void CP_Write16(uint32_t off, uint16_t v);
uint16_t PE_Read16(uint32_t off);
void PE_Write16(uint32_t off, uint16_t v);
uint16_t VI_Read16(uint32_t off);
void VI_Write16(uint32_t off, uint16_t v);
uint32_t PI_Read32(uint32_t off);
void PI_Write32(uint32_t off, uint32_t v);
uint16_t MI_Read16(uint32_t off);
void MI_Write16(uint32_t off, uint16_t v);
uint16_t DSP_Read16(uint32_t off);
void DSP_Write16(uint32_t off, uint16_t v);
uint32_t DSP_Read32(uint32_t off);
void DSP_Write32(uint32_t off, uint32_t v);
uint32_t SI_Read32(uint32_t off);
void SI_Write32(uint32_t off, uint32_t v);
uint32_t EXI_Read32(uint32_t off);
void EXI_Write32(uint32_t off, uint32_t v);
uint32_t AI_Read32(uint32_t off);
void AI_Write32(uint32_t off, uint32_t v);
uint32_t HLW_Read32(uint32_t off);
void HLW_Write32(uint32_t off, uint32_t v);
uint32_t DI_Read32(uint32_t off);
void DI_Write32(uint32_t off, uint32_t v);

// GX write-gather pipe
void GPFifo_Write8(uint8_t v);
void GPFifo_Write16(uint16_t v);
void GPFifo_Write32(uint32_t v);

// EFB peek/poke (0xC8000000)
// Waits for the GX thread to drain; the lock keeps it idle while held.
#include <mutex>
std::unique_lock<std::mutex> GX_SyncGPU();
void GX_StartThread();
uint32_t EFB_Read32(uint32_t ea);
void EFB_Write32(uint32_t ea, uint32_t v);

// module init
void VI_Init();
void EXI_Init();
void DSP_Init();
void SI_Init();
void IPC_Init();
void GX_Init();

// IPC (used by IOS)
void IPC_GenerateReply(uint32_t addr);
void IPC_GenerateAck();
