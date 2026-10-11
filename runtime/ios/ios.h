#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// IOS IPC request (guest memory layout at `addr`)
enum IPCCommand : uint32_t
{
    IPC_CMD_OPEN = 1,
    IPC_CMD_CLOSE = 2,
    IPC_CMD_READ = 3,
    IPC_CMD_WRITE = 4,
    IPC_CMD_SEEK = 5,
    IPC_CMD_IOCTL = 6,
    IPC_CMD_IOCTLV = 7,
    IPC_REPLY = 8,
};

enum : int32_t
{
    IPC_SUCCESS = 0,
    IPC_EACCES = -1,
    IPC_EEXIST = -2,
    IPC_EINVAL = -4,
    IPC_ENOENT = -6,
    FS_EINVAL = -101,
    FS_EACCES = -102,
    FS_EEXIST = -105,
    FS_ENOENT = -106,
    FS_ENFILE = -107,
    ES_EINVAL = -1017,
};

struct IOVec
{
    uint32_t addr;   // guest physical
    uint32_t size;
};

struct IOSRequest
{
    uint32_t addr;
    uint32_t cmd;
    int32_t fd;
    // ioctl
    uint32_t ioctl;
    uint32_t inAddr, inSize, outAddr, outSize;
    std::vector<IOVec> in, io;
};

// Reply semantics: return std::nullopt-like via `pending` to delay the reply.
struct IOSReply
{
    int32_t value = 0;
    bool pending = false;     // reply will be sent later by the device
    uint64_t delayTicks = 200;
};

class IOSDevice
{
public:
    explicit IOSDevice(std::string name) : m_name(std::move(name)) {}
    virtual ~IOSDevice() = default;
    const std::string& Name() const { return m_name; }
    virtual IOSReply Open(const std::string& path, uint32_t mode) { return { IPC_SUCCESS }; }
    virtual IOSReply Close() { return { IPC_SUCCESS }; }
    virtual IOSReply Read(uint32_t addr, uint32_t size) { return { IPC_EINVAL }; }
    virtual IOSReply Write(uint32_t addr, uint32_t size) { return { IPC_EINVAL }; }
    virtual IOSReply Seek(int32_t offset, uint32_t whence) { return { IPC_EINVAL }; }
    virtual IOSReply Ioctl(const IOSRequest& r);
    virtual IOSReply Ioctlv(const IOSRequest& r);

protected:
    std::string m_name;
};

void IOS_Init(const std::string& nandRoot);
void IOS_EnqueueRequest(uint32_t addr);
void IOS_SendDelayedReply(uint32_t requestAddr, int32_t value, uint64_t delayTicks = 0);
void IOS_UpdateIPC();
const std::string& IOS_NandRoot();

// Device factories
std::shared_ptr<IOSDevice> MakeDIDevice();
std::shared_ptr<IOSDevice> MakeESDevice();
std::shared_ptr<IOSDevice> MakeFSDevice();
std::shared_ptr<IOSDevice> MakeNandFile(const std::string& path, uint32_t mode, int32_t* error);
std::shared_ptr<IOSDevice> MakeSTMImmediate();
std::shared_ptr<IOSDevice> MakeSTMEventHook();
std::shared_ptr<IOSDevice> MakeStubDevice(const std::string& name);

// Helpers
std::string ReadGuestString(uint32_t physAddr, uint32_t maxLen = 64);
