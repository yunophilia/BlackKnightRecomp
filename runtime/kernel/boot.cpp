// HLE of the Wii boot path (IOS memory setup + BS2 + apploader effects).
#include "boot.h"
#include "memory.h"
#include "../ios/disc.h"
#include "../log.h"

#include <vector>

std::vector<uint8_t> BuildSettingTxt();

static std::vector<uint8_t> LoadHostFile(const std::string& p)
{
    std::vector<uint8_t> v;
    FILE* f = nullptr;
    if (!fopen_s(&f, p.c_str(), "rb") && f)
    {
        fseek(f, 0, SEEK_END);
        v.resize(ftell(f));
        fseek(f, 0, SEEK_SET);
        fread(v.data(), 1, v.size(), f);
        fclose(f);
    }
    return v;
}

static inline uint32_t BE32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

uint32_t Boot_Setup(const std::string& dataDir)
{
    const uint32_t L = 0x80000000;
    memset(GuestPtr(L), 0, 0x4000);

    // --- IOS55 memory values
    Write32(L | 0x3100, MEM1_SIZE);
    Write32(L | 0x3104, MEM1_SIZE);
    Write32(L | 0x3108, 0x81800000);
    Write32(L | 0x310C, 0x00000000);
    Write32(L | 0x3110, 0x81800000);
    Write32(L | 0x3114, 0xDEADBEEF);
    Write32(L | 0x3118, MEM2_SIZE);
    Write32(L | 0x311C, MEM2_SIZE);
    Write32(L | 0x3120, 0x93600000);
    Write32(L | 0x3124, 0x90000800);
    Write32(L | 0x3128, 0x935E0000);
    Write32(L | 0x312C, 0xDEADBEEF);
    Write32(L | 0x3130, 0x935E0000);
    Write32(L | 0x3134, 0x93600000);
    Write32(L | 0x3138, 0x00000011);
    Write32(L | 0x313C, 0xDEADBEEF);
    Write32(L | 0x3140, 0x0037161F);
    Write32(L | 0x3144, 0x00030110);
    Write32(L | 0x3148, 0x93600000);
    Write32(L | 0x314C, 0x93620000);
    Write32(L | 0x3150, 0xDEADBEEF);
    Write32(L | 0x3154, 0xDEADBEEF);
    Write32(L | 0x3158, 0x0000FF01);
    Write8(L | 0x315D, 0xAD);
    Write32(L | 0x3160, 0);
    Write32(L | 0x0028, MEM1_SIZE);
    Write32(L | 0x00F0, MEM1_SIZE);

    // --- System menu / BS2 globals (Dolphin SetupWiiMemory)
    Write32(L | 0x0020, 0x0D15EA5E);
    Write32(L | 0x0024, 0x00000001);
    Write32(L | 0x002C, 0x00000023);   // RVL retail 3
    Write32(L | 0x0030, 0x00000000);
    Write32(L | 0x0034, 0x817FEC60);
    Write32(L | 0x00E4, 0x8008F7B8);
    Write32(L | 0x00F4, 0x8179B500);
    Write32(L | 0x00F8, 0x0E7BE2C0);   // bus speed
    Write32(L | 0x00FC, 0x2B73A840);   // CPU speed
    Write32(L | 0x30C0, 0);
    Write32(L | 0x30C4, 0);
    Write32(L | 0x30DC, 0);
    Write32(L | 0x30D8, 0xFFFFFFFF);
    Write16(L | 0x30E6, 0x8201);
    Write32(L | 0x30F0, 0);
    Write16(L | 0x315E, 0x0113);
    Write8(L | 0x315C, 0x80);
    Write16(L | 0x30E0, 0);
    Write32(L | 0x3184, 0x80000000);
    Write32(L | 0x3188, 0x0037161F);   // IOS version required by the title (set by the apploader)
    Write32(L | 0x00CC, 1);            // PAL
    auto setting = BuildSettingTxt();
    memcpy(GuestPtr(L | 0x3800), setting.data(), setting.size());
    Write32(L | 0x3194, 0);
    Write32(L | 0x3198, (uint32_t)(Disc_PartitionOffset() >> 2));

    // disc id
    memcpy(GuestPtr(L), Disc_Header(), 0x20);
    memcpy(GuestPtr(L | 0x3180), Disc_Header(), 4);

    // --- apploader: load the DOL
    std::vector<uint8_t> dol = LoadHostFile(dataDir + "/sys/main.dol");
    if (dol.size() < 0x100) { LOG_ERROR("main.dol missing"); return 0; }
    for (int i = 0; i < 18; i++)
    {
        uint32_t off = BE32(&dol[i * 4]);
        uint32_t addr = BE32(&dol[0x48 + i * 4]);
        uint32_t size = BE32(&dol[0x90 + i * 4]);
        if (size)
            memcpy(GuestPtr(addr), dol.data() + off, size);
    }
    uint32_t bss = BE32(&dol[0xD8]);
    uint32_t bssSize = BE32(&dol[0xDC]);
    uint32_t entry = BE32(&dol[0xE0]);
    // BSS overlaps sbss/sdata sections in DOLs: only clear ranges not covered by data sections
    for (uint32_t a = bss; a < bss + bssSize; a += 4)
    {
        bool covered = false;
        for (int i = 0; i < 18 && !covered; i++)
        {
            uint32_t sa = BE32(&dol[0x48 + i * 4]), ss = BE32(&dol[0x90 + i * 4]);
            covered = ss && a >= sa && a < sa + ss;
        }
        if (!covered) Write32(a, 0);
    }

    // --- FST and BI2 placed at the top of MEM1
    std::vector<uint8_t> fst = LoadHostFile(dataDir + "/sys/fst.bin");
    std::vector<uint8_t> bi2 = LoadHostFile(dataDir + "/sys/bi2.bin");
    const uint8_t* hdr = Disc_Header();
    uint32_t fstMax = BE32(hdr + 0x42C) << 2;
    if (fstMax < fst.size()) fstMax = (uint32_t)fst.size();
    uint32_t fstAddr = (0x817FEC60 - fstMax) & ~0x1Fu;
    memcpy(GuestPtr(fstAddr), fst.data(), fst.size());
    Write32(L | 0x0038, fstAddr);
    Write32(L | 0x003C, fstMax);
    uint32_t bi2Addr = (fstAddr - 0x2000) & ~0x1Fu;
    if (!bi2.empty()) memcpy(GuestPtr(bi2Addr), bi2.data(), std::min<size_t>(bi2.size(), 0x2000));
    Write32(L | 0x00F4, bi2Addr);
    Write32(L | 0x0034, bi2Addr);
    Write32(L | 0x3110, bi2Addr);

    LOG_INFO("boot: DOL entry {:08x}, FST at {:08x} ({} bytes), arena hi {:08x}", entry, fstAddr, fst.size(), bi2Addr);
    return entry;
}
