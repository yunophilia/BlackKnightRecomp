// /dev/di : disc drive interface
#include "ios.h"
#include "disc.h"
#include "../kernel/memory.h"
#include "../log.h"

#include <vector>

enum DIResult : int32_t
{
    DI_SUCCESS = 1,
    DI_DRIVE_ERROR = 2,
    DI_COVER_CLOSED = 4,
    DI_SECURITY_ERROR = 32,
    DI_BAD_ARGUMENT = 128,
};

static bool s_partitionOpen = true;
static uint32_t s_lastError = 0;   // the system menu opened it before launching us

static uint64_t ReadTicks(uint32_t len)
{
    // ~10 MB/s plus fixed latency: keeps loading sequences in a realistic order
    return 6000 + (uint64_t)len * 6;
}

class DIDevice : public IOSDevice
{
public:
    DIDevice() : IOSDevice("/dev/di") {}

    IOSReply Ioctl(const IOSRequest& r) override
    {
        uint32_t cmd = Read32(0x80000000 | r.inAddr) >> 24;
        uint32_t in = 0x80000000 | r.inAddr;
        LOG_DEBUG("DI ioctl {:02x} in[{:08x} {:08x} {:08x}] out {:x}", cmd, Read32(in), Read32(in + 4), Read32(in + 8), r.outSize);
        switch (cmd)
        {
        case 0x12:   // inquiry
            memset(PhysPtr(r.outAddr), 0, r.outSize);
            if (r.outSize >= 8) { Write32(0x80000000 | r.outAddr, 0x00000002); Write32(0x80000000 | (r.outAddr + 4), 0x20060526); }
            return { DI_SUCCESS };
        case 0x70:   // read disk id
            memcpy(PhysPtr(r.outAddr), Disc_Header(), std::min<uint32_t>(r.outSize, 0x20));
            return { DI_SUCCESS };
        case 0x71:   // read (decrypted, partition-relative, offset >> 2)
        {
            uint32_t len = Read32(in + 4);
            uint64_t pos = (uint64_t)Read32(in + 8) << 2;
            if (!s_partitionOpen) return { DI_SECURITY_ERROR };
            if (r.outSize < len) return { DI_SECURITY_ERROR };
            Disc_ReadPartition(pos, len, PhysPtr(r.outAddr));
            return { DI_SUCCESS, false, ReadTicks(len) };
        }
        case 0x79:   // wait for cover close
            return { DI_COVER_CLOSED };
        case 0x7A:   // get cover register
            if (r.outSize >= 4) Write32(0x80000000 | r.outAddr, 0);
            return { DI_SUCCESS };
        case 0x7E:   // notify reset
            return { DI_SUCCESS };
        case 0x83: case 0x84:
            if (r.outSize >= 4) Write32(0x80000000 | r.outAddr, 0);
            return { DI_SUCCESS };
        case 0x85: case 0x86: case 0x87: case 0x89:
            return { DI_SUCCESS };
        case 0x88:   // get cover status: disc inserted
            if (r.outSize >= 4) Write32(0x80000000 | r.outAddr, 2);
            return { DI_SUCCESS };
        case 0x8A:   // reset
            return { DI_SUCCESS };
        case 0x8C:   // close partition
            s_partitionOpen = false;
            return { DI_SUCCESS };
        case 0x8D:   // unencrypted read (raw disc)
        {
            uint32_t len = Read32(in + 4);
            uint32_t pos32 = Read32(in + 8);
            if (pos32 >= 0x14000)
            {
                // Out-of-bounds probes (anti-modchip "error #001" check): a single layer
                // disc must fail with "logical block address out of range".
                s_lastError = 0x00052100;
                return { DI_DRIVE_ERROR, false, 20000 };
            }
            uint64_t pos = (uint64_t)pos32 << 2;
            Disc_ReadRaw(pos, len, PhysPtr(r.outAddr));
            return { DI_SUCCESS, false, ReadTicks(len) };
        }
        case 0x8E:   // enable DVD video
            return { DI_SUCCESS };
        case 0x95: case 0x96:   // status / control register
            if (r.outSize >= 4) Write32(0x80000000 | r.outAddr, 0);
            return { DI_SUCCESS };
        case 0xA4:   // report key: rejected by retail drives (used as a hardware check)
            s_lastError = 0x00052000;
            return { DI_DRIVE_ERROR };
        case 0xAB:   // seek
            return { DI_SUCCESS };
        case 0xD0: case 0xD1: case 0xDA:
            return { DI_DRIVE_ERROR };
        case 0xDB:   // request disc status
        case 0xDC:   // request retry number
            if (r.outSize >= 4) Write32(0x80000000 | r.outAddr, 0);
            return { DI_SUCCESS };
        case 0xE0:   // request error
            if (r.outSize >= 4) Write32(0x80000000 | r.outAddr, s_lastError);
            s_lastError = 0;
            return { DI_SUCCESS };
        case 0xE3:   // stop motor
            return { DI_SUCCESS };
        case 0xE4:   // audio buffer config
            return { DI_SUCCESS };
        default:
            LOG_WARN("DI: unhandled ioctl command {:02x}", cmd);
            return { DI_SUCCESS };
        }
    }

    IOSReply Ioctlv(const IOSRequest& r) override
    {
        uint32_t cmd = r.in.empty() ? 0 : Read32(0x80000000 | r.in[0].addr) >> 24;
        LOG_INFO("DI ioctlv {:02x}", cmd);
        switch (cmd)
        {
        case 0x8B:   // open partition: out[0] = TMD, out[1] = error code
        {
            s_partitionOpen = true;
            std::vector<uint8_t> tmd;
            FILE* f = nullptr;
            std::string p = Disc_DataDir() + "/tmd.bin";
            if (!fopen_s(&f, p.c_str(), "rb") && f)
            {
                fseek(f, 0, SEEK_END);
                tmd.resize(ftell(f));
                fseek(f, 0, SEEK_SET);
                fread(tmd.data(), 1, tmd.size(), f);
                fclose(f);
            }
            if (!r.io.empty())
                memcpy(PhysPtr(r.io[0].addr), tmd.data(), std::min<size_t>(tmd.size(), r.io[0].size));
            if (r.io.size() > 1 && r.io[1].size >= 4)
                Write32(0x80000000 | r.io[1].addr, 0);
            return { DI_SUCCESS };
        }
        default:
            LOG_WARN("DI: unhandled ioctlv command {:02x}", cmd);
            return { DI_BAD_ARGUMENT };
        }
    }
};

std::shared_ptr<IOSDevice> MakeDIDevice() { return std::make_shared<DIDevice>(); }
