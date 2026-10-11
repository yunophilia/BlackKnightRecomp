// Expansion Interface: only the IPL device (RTC / SRAM / UART) is present.
#include "hw.h"
#include "../kernel/memory.h"
#include "../log.h"

#include <ctime>
#include <string>

struct IPLDevice
{
    uint32_t cmd = 0;
    int cmdBytes = 0;
    uint32_t cursor = 0;
    uint8_t sram[0x44] = {};
    uint8_t rtcFlags = 0;
    std::string uart;

    void Reset()
    {
        cmd = 0;
        cmdBytes = 0;
        cursor = 0;
    }

    void UpdateRTC()
    {
        // seconds since 2000-01-01 in local time
        time_t now = time(nullptr);
        struct tm lt;
        localtime_s(&lt, &now);
        uint32_t rtc = (uint32_t)(_mkgmtime(&lt) - 946684800);
        sram[0] = rtc >> 24; sram[1] = rtc >> 16; sram[2] = rtc >> 8; sram[3] = rtc;
    }

    void TransferByte(uint8_t& data)
    {
        if (cmdBytes < 4)
        {
            cmd = (cmd << 8) | data;
            data = 0xFF;
            if (++cmdBytes == 4)
                UpdateRTC();
            return;
        }
        bool write = (cmd >> 31) & 1;
        uint32_t addr = (cmd >> 6) & 0x1FFFFFF;
        if (addr < 0x200000)
        {
            if (!write) data = 0;   // no IPL ROM (fonts come from the disc/NAND on Wii)
        }
        else if (addr >= 0x800000 && addr < 0x800000 + 0x44)
        {
            uint32_t a = (addr - 0x800000 + cursor++) % 0x44;
            if (write) sram[a] = data;
            else data = sram[a];
        }
        else if (addr >= 0x800400 && addr < 0x800450)
        {
            if (addr == 0x800400)
            {
                if (write)
                {
                    if (data == '\r' || data == '\n')
                    {
                        if (!uart.empty()) LOG_INFO("[UART] {}", uart);
                        uart.clear();
                    }
                    else if (data) uart += (char)data;
                }
                else data = 0;
            }
        }
        else if (addr >= 0x840000 && addr < 0x840040 && addr - 0x840000 == 0x20)
        {
            if (write) rtcFlags = data;
            else data = rtcFlags;
        }
        else if (addr >= 0xC00000 && addr < 0xC00008)
        {
            if (addr == 0xC00004 && write)
            {
                if (data == '\r' || data == '\n') { if (!uart.empty()) LOG_INFO("[UART] {}", uart); uart.clear(); }
                else if (data) uart += (char)data;
            }
        }
    }
};

struct Channel
{
    uint32_t csr = 0;
    uint32_t mar = 0;
    uint32_t len = 0;
    uint32_t cr = 0;
    uint32_t data = 0;
};

static Channel s_ch[3];
static IPLDevice s_ipl;

static void UpdateInterrupts()
{
    bool any = false;
    for (auto& c : s_ch)
    {
        if (((c.csr & 2) && (c.csr & 1)) || ((c.csr & 8) && (c.csr & 4)) || ((c.csr & 0x800) && (c.csr & 0x400)))
            any = true;
    }
    PI_SetInterrupt(INT_CAUSE_EXI, any);
}

static IPLDevice* SelectedDevice(int ch)
{
    uint32_t cs = (s_ch[ch].csr >> 7) & 7;
    if (ch == 0 && cs == 2)
        return &s_ipl;
    return nullptr;
}

uint32_t EXI_Read32(uint32_t off)
{
    int ch = off / 0x14;
    uint32_t reg = off % 0x14;
    if (ch > 2) return 0;
    Channel& c = s_ch[ch];
    switch (reg)
    {
    case 0x00:
    {
        uint32_t v = c.csr & ~0x1000u;
        // EXT: device present in slot (only channel 0 device 0 / channel 1); no memory cards
        return v;
    }
    case 0x04: return c.mar;
    case 0x08: return c.len;
    case 0x0C: return c.cr;
    case 0x10: return c.data;
    }
    return 0;
}

void EXI_Write32(uint32_t off, uint32_t v)
{
    int ch = off / 0x14;
    uint32_t reg = off % 0x14;
    if (ch > 2) return;
    Channel& c = s_ch[ch];
    switch (reg)
    {
    case 0x00:
    {
        uint32_t oldCs = (c.csr >> 7) & 7;
        // interrupt status bits are write-1-to-clear
        uint32_t clear = v & (2 | 8 | 0x800);
        c.csr = (c.csr & ~clear & (2 | 8 | 0x800)) | (v & ~(2u | 8u | 0x800u));
        uint32_t newCs = (c.csr >> 7) & 7;
        if (oldCs != newCs && ch == 0)
            s_ipl.Reset();
        UpdateInterrupts();
        break;
    }
    case 0x04: c.mar = v & 0x03FFFFE0; break;
    case 0x08: c.len = v & 0x03FFFFE0; break;
    case 0x10: c.data = v; break;
    case 0x0C:
    {
        c.cr = v;
        if (!(v & 1)) break;
        IPLDevice* dev = SelectedDevice(ch);
        uint32_t rw = (v >> 2) & 3;
        if (v & 2)
        {
            // DMA
            uint8_t* p = PhysPtr(c.mar);
            for (uint32_t i = 0; i < c.len; i++)
            {
                uint8_t b = (rw == 1) ? p[i] : 0;
                if (dev) dev->TransferByte(b);
                else b = 0xFF;
                if (rw == 0) p[i] = b;
            }
        }
        else
        {
            uint32_t n = ((v >> 4) & 3) + 1;
            uint32_t out = 0;
            for (uint32_t i = 0; i < n; i++)
            {
                uint8_t b = (uint8_t)(c.data >> (24 - 8 * i));
                if (dev) dev->TransferByte(b);
                else b = 0xFF;
                out |= (uint32_t)b << (24 - 8 * i);
            }
            if (rw != 1)
                c.data = out;
        }
        c.cr &= ~1u;
        c.csr |= 8;   // transfer complete
        UpdateInterrupts();
        break;
    }
    }
}

void EXI_Init()
{
    // SRAM defaults (from Dolphin): checksum, OOBE done, stereo
    uint8_t* s = s_ipl.sram;
    memset(s, 0, sizeof(s_ipl.sram));
    s[4] = 0x00; s[5] = 0x2C; s[6] = 0xFF; s[7] = 0xD0;
    s[0x17] = 0x20 | 0x08 | 0x04;   // flags: OOBE done, stereo
    memcpy(s + 0x18, "DOLPHINSLOTA", 12);
    memcpy(s + 0x24, "DOLPHINSLOTB", 12);
    s[0x3E] = 0x6E; s[0x3F] = 0x6D;
}
