// IOS kernel HLE: IPC request queue and device dispatch.
#include "ios.h"
#include "../hw/hw.h"
#include "../kernel/memory.h"
#include "../kernel/scheduler.h"
#include "../log.h"

#include <deque>
#include <map>

static std::string s_nandRoot;
static std::shared_ptr<IOSDevice> s_fds[32];
static std::deque<uint32_t> s_requestQueue;
static std::deque<uint32_t> s_replyQueue;
static int s_evtEnqueue = -1;
static int s_evtReply = -1;

bool IPC_IsReady();
void IPC_ClearX1();

const std::string& IOS_NandRoot() { return s_nandRoot; }

std::string ReadGuestString(uint32_t phys, uint32_t maxLen)
{
    const char* p = (const char*)PhysPtr(phys);
    size_t n = strnlen(p, maxLen);
    return std::string(p, n);
}

IOSReply IOSDevice::Ioctl(const IOSRequest& r)
{
    LOG_WARN("IOS: {} unhandled ioctl {:x} (in {:x}/{:x} out {:x}/{:x})", m_name, r.ioctl, r.inAddr, r.inSize, r.outAddr, r.outSize);
    return { IPC_EINVAL };
}

IOSReply IOSDevice::Ioctlv(const IOSRequest& r)
{
    LOG_WARN("IOS: {} unhandled ioctlv {:x} ({} in, {} io)", m_name, r.ioctl, r.in.size(), r.io.size());
    return { IPC_EINVAL };
}

// ------------------------------------------------------------------ stub device
class StubDevice : public IOSDevice
{
public:
    using IOSDevice::IOSDevice;
    IOSReply Ioctl(const IOSRequest& r) override
    {
        LOG_INFO("IOS: stub {} ioctl {:x}", m_name, r.ioctl);
        return { IPC_SUCCESS };
    }
    IOSReply Ioctlv(const IOSRequest& r) override
    {
        LOG_INFO("IOS: stub {} ioctlv {:x}", m_name, r.ioctl);
        return { IPC_SUCCESS };
    }
};
std::shared_ptr<IOSDevice> MakeStubDevice(const std::string& name) { return std::make_shared<StubDevice>(name); }

// ------------------------------------------------------------------ open
static std::shared_ptr<IOSDevice> OpenDevice(const std::string& path, uint32_t mode, int32_t* err)
{
    *err = IPC_SUCCESS;
    if (path == "/dev/di") return MakeDIDevice();
    if (path == "/dev/es") return MakeESDevice();
    if (path == "/dev/fs") return MakeFSDevice();
    if (path == "/dev/stm/immediate") return MakeSTMImmediate();
    if (path == "/dev/stm/eventhook") return MakeSTMEventHook();
    if (path.rfind("/dev/", 0) == 0)
    {
        // Devices we do not provide (Bluetooth, network, USB, SD...): report absent.
        LOG_INFO("IOS: open {} -> ENOENT", path);
        *err = IPC_ENOENT;
        return nullptr;
    }
    // NAND file
    return MakeNandFile(path, mode, err);
}

// ------------------------------------------------------------------ request handling
static void ExecuteRequest(uint32_t addr)
{
    IOSRequest r{};
    r.addr = addr;
    r.cmd = Read32(0x80000000 | addr);
    r.fd = (int32_t)Read32(0x80000000 | (addr + 8));
    uint32_t a = 0x80000000 | addr;
    IOSReply reply;
    std::shared_ptr<IOSDevice> dev = (r.fd >= 0 && r.fd < 32) ? s_fds[r.fd] : nullptr;

    switch (r.cmd)
    {
    case IPC_CMD_OPEN:
    {
        std::string path = ReadGuestString(Read32(a + 0x0C));
        uint32_t mode = Read32(a + 0x10);
        int fd = -1;
        for (int i = 0; i < 32; i++)
            if (!s_fds[i]) { fd = i; break; }
        if (fd < 0) { reply.value = FS_ENFILE; break; }
        int32_t err;
        auto d = OpenDevice(path, mode, &err);
        if (!d) { reply.value = err; LOG_INFO("IOS: open {} failed ({})", path, err); break; }
        IOSReply o = d->Open(path, mode);
        if (o.value < 0) { reply = o; break; }
        s_fds[fd] = d;
        reply.value = fd;
        LOG_INFO("IOS: open {} (mode {}) -> fd {}", path, mode, fd);
        break;
    }
    case IPC_CMD_CLOSE:
        if (!dev) { reply.value = IPC_EINVAL; break; }
        reply = dev->Close();
        s_fds[r.fd].reset();
        break;
    case IPC_CMD_READ:
        if (!dev) { reply.value = IPC_EINVAL; break; }
        reply = dev->Read(Read32(a + 0x0C), Read32(a + 0x10));
        break;
    case IPC_CMD_WRITE:
        if (!dev) { reply.value = IPC_EINVAL; break; }
        reply = dev->Write(Read32(a + 0x0C), Read32(a + 0x10));
        break;
    case IPC_CMD_SEEK:
        if (!dev) { reply.value = IPC_EINVAL; break; }
        reply = dev->Seek((int32_t)Read32(a + 0x0C), Read32(a + 0x10));
        break;
    case IPC_CMD_IOCTL:
        if (!dev) { reply.value = IPC_EINVAL; break; }
        r.ioctl = Read32(a + 0x0C);
        r.inAddr = Read32(a + 0x10);
        r.inSize = Read32(a + 0x14);
        r.outAddr = Read32(a + 0x18);
        r.outSize = Read32(a + 0x1C);
        reply = dev->Ioctl(r);
        break;
    case IPC_CMD_IOCTLV:
    {
        if (!dev) { reply.value = IPC_EINVAL; break; }
        r.ioctl = Read32(a + 0x0C);
        uint32_t nIn = Read32(a + 0x10);
        uint32_t nIo = Read32(a + 0x14);
        uint32_t vec = Read32(a + 0x18);
        for (uint32_t i = 0; i < nIn + nIo; i++)
        {
            IOVec v{ Read32(0x80000000 | (vec + 8 * i)), Read32(0x80000000 | (vec + 8 * i + 4)) };
            (i < nIn ? r.in : r.io).push_back(v);
        }
        reply = dev->Ioctlv(r);
        break;
    }
    default:
        LOG_ERROR("IOS: unknown IPC command {} at {:08x}", r.cmd, addr);
        reply.value = IPC_EINVAL;
        break;
    }
    if (!reply.pending)
        IOS_SendDelayedReply(addr, reply.value, reply.delayTicks);
}

void IOS_SendDelayedReply(uint32_t addr, int32_t value, uint64_t delayTicks)
{
    uint32_t a = 0x80000000 | addr;
    uint32_t cmd = Read32(a);
    Write32(a + 4, (uint32_t)value);
    Write32(a + 8, cmd);
    Write32(a, IPC_REPLY);
    Sched_ScheduleIn(s_evtReply, delayTicks, addr);
}

static void OnEnqueue(uint64_t addr, int64_t)
{
    s_requestQueue.push_back((uint32_t)addr);
    IOS_UpdateIPC();
}

static void OnReply(uint64_t addr, int64_t)
{
    s_replyQueue.push_back((uint32_t)addr);
    IOS_UpdateIPC();
}

void IOS_EnqueueRequest(uint32_t addr)
{
    Sched_ScheduleIn(s_evtEnqueue, 500, addr);
}

void IOS_UpdateIPC()
{
    if (!IPC_IsReady())
        return;
    if (!s_requestQueue.empty())
    {
        uint32_t addr = s_requestQueue.front();
        s_requestQueue.pop_front();
        IPC_ClearX1();
        IPC_GenerateAck();
        ExecuteRequest(addr);
        return;
    }
    if (!s_replyQueue.empty())
    {
        uint32_t addr = s_replyQueue.front();
        s_replyQueue.pop_front();
        IPC_GenerateReply(addr);
    }
}

void IOS_Init(const std::string& nandRoot)
{
    s_nandRoot = nandRoot;
    s_evtEnqueue = Sched_RegisterEvent("IOSEnqueue", OnEnqueue);
    s_evtReply = Sched_RegisterEvent("IOSReply", OnReply);
}
