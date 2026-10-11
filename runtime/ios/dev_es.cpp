// /dev/es : ticket/title services (only what a disc title needs)
#include "ios.h"
#include "disc.h"
#include "../kernel/memory.h"
#include "../log.h"

#include <vector>
#include <windows.h>

// The console is leaving the game: "Wii Menu" from the HOME menu (ES_LaunchTitle)
// or power off simply quit; "Reset" (STM hot reset) restarts the executable.
[[noreturn]] static void LeaveGame(bool restart)
{
    LOG_INFO("{}", restart ? "game requested reset: restarting" : "game requested exit");
    if (restart)
    {
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (CreateProcessW(exe, GetCommandLineW(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    ExitProcess(0);
}

static constexpr uint64_t TITLE_ID = 0x00010000'52454E45ull;   // RENE

static std::vector<uint8_t> LoadFile(const std::string& p)
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

class ESDevice : public IOSDevice
{
public:
    ESDevice() : IOSDevice("/dev/es") {}

    IOSReply Ioctlv(const IOSRequest& r) override
    {
        auto out32 = [&](size_t i, uint32_t v) { if (r.io.size() > i && r.io[i].size >= 4) Write32(0x80000000 | r.io[i].addr, v); };
        auto out64 = [&](size_t i, uint64_t v) { if (r.io.size() > i && r.io[i].size >= 8) Write64(0x80000000 | r.io[i].addr, v); };
        switch (r.ioctl)
        {
        case 0x08:   // LaunchTitle (return to the Wii Menu)
            LeaveGame(false);
        case 0x07:   // GetDeviceID
            out32(0, 0x0403AC68);
            return { IPC_SUCCESS };
        case 0x20:   // GetTitleID
            out64(0, TITLE_ID);
            return { IPC_SUCCESS };
        case 0x1D:   // GetTitleDir
        {
            uint64_t tid = r.in.empty() ? TITLE_ID : Read64(0x80000000 | r.in[0].addr);
            char buf[32];
            snprintf(buf, sizeof(buf), "/title/%08x/%08x/data", (uint32_t)(tid >> 32), (uint32_t)tid);
            if (!r.io.empty())
                memcpy(PhysPtr(r.io[0].addr), buf, std::min<size_t>(strlen(buf) + 1, r.io[0].size));
            return { IPC_SUCCESS };
        }
        case 0x16:   // GetConsumption: no limits
            out32(1, 0);
            return { IPC_SUCCESS };
        case 0x12:   // GetTicketViewCount
            out32(0, 1);
            return { IPC_SUCCESS };
        case 0x13:   // GetTicketViews
        {
            if (!r.io.empty())
            {
                std::vector<uint8_t> tik = LoadFile(Disc_DataDir() + "/ticket.bin");
                uint8_t* o = PhysPtr(r.io[0].addr);
                memset(o, 0, r.io[0].size);
                // ticket view: view id(4) + ticket fields from 0x1D0
                if (tik.size() >= 0x2A4 && r.io[0].size >= 0xD8)
                    memcpy(o + 4, tik.data() + 0x1D0, 0xD4);
            }
            return { IPC_SUCCESS };
        }
        case 0x14:   // GetTMDViewCount
        {
            std::vector<uint8_t> tmd = LoadFile(Disc_DataDir() + "/tmd.bin");
            out32(0, (uint32_t)(0x5C + 2 + 0x10 * (tmd.size() >= 0x1E0 ? ((tmd[0x1DE] << 8) | tmd[0x1DF]) : 0)));
            return { IPC_SUCCESS };
        }
        case 0x1B:   // DIGetTicketView
        {
            if (!r.io.empty())
            {
                std::vector<uint8_t> tik = LoadFile(Disc_DataDir() + "/ticket.bin");
                uint8_t* o = PhysPtr(r.io[0].addr);
                memset(o, 0, r.io[0].size);
                if (tik.size() >= 0x2A4 && r.io[0].size >= 0xD8)
                    memcpy(o + 4, tik.data() + 0x1D0, 0xD4);
            }
            return { IPC_SUCCESS };
        }
        case 0x39:   // DIGetTMDSize
        {
            std::vector<uint8_t> tmd = LoadFile(Disc_DataDir() + "/tmd.bin");
            out32(0, (uint32_t)tmd.size());
            return { IPC_SUCCESS };
        }
        case 0x3A:   // DIGetTMD
        {
            std::vector<uint8_t> tmd = LoadFile(Disc_DataDir() + "/tmd.bin");
            if (!r.io.empty())
                memcpy(PhysPtr(r.io[0].addr), tmd.data(), std::min<size_t>(tmd.size(), r.io[0].size));
            return { IPC_SUCCESS };
        }
        case 0x1C:   // DIVerify
            return { IPC_SUCCESS };
        default:
            return IOSDevice::Ioctlv(r);
        }
    }
};

std::shared_ptr<IOSDevice> MakeESDevice() { return std::make_shared<ESDevice>(); }

// ------------------------------------------------------------------ STM
class STMImmediate : public IOSDevice
{
public:
    STMImmediate() : IOSDevice("/dev/stm/immediate") {}
    IOSReply Ioctl(const IOSRequest& r) override
    {
        switch (r.ioctl)
        {
        case 0x2001: case 0x2002: LeaveGame(true);    // hot reset
        case 0x2003: LeaveGame(false);                // shutdown
        case 0x3001: return { IPC_SUCCESS };   // get idle mode
        case 0x3002: return { IPC_SUCCESS };   // release event hook
        case 0x4001: return { IPC_SUCCESS };   // unk
        case 0x5001: return { IPC_SUCCESS };   // VI dimming
        case 0x6001: case 0x6002: return { IPC_SUCCESS };
        case 0x7001: return { IPC_SUCCESS };   // LED
        default: return { IPC_SUCCESS };
        }
    }
};

class STMEventHook : public IOSDevice
{
public:
    STMEventHook() : IOSDevice("/dev/stm/eventhook") {}
    IOSReply Ioctl(const IOSRequest& r) override
    {
        if (r.ioctl == 0x1000)
        {
            // Event hook: replied to only when the power/reset button is pressed.
            IOSReply rep;
            rep.pending = true;
            return rep;
        }
        return { IPC_SUCCESS };
    }
};

std::shared_ptr<IOSDevice> MakeSTMImmediate() { return std::make_shared<STMImmediate>(); }
std::shared_ptr<IOSDevice> MakeSTMEventHook() { return std::make_shared<STMEventHook>(); }
