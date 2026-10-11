// /dev/fs and NAND file access, backed by a host directory.
#include "../config.h"
#include "ios.h"
#include "../kernel/memory.h"
#include "../log.h"

#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

static fs::path HostPath(const std::string& nandPath)
{
    std::string p = nandPath;
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    return fs::path(IOS_NandRoot()) / fs::u8path(p);
}

static bool ValidPath(const std::string& p)
{
    return !p.empty() && p[0] == '/' && p.find("..") == std::string::npos;
}

// ------------------------------------------------------------------ file handle
class NandFile : public IOSDevice
{
public:
    NandFile(const std::string& path, uint32_t mode) : IOSDevice(path), m_mode(mode) {}

    bool Load(int32_t* err)
    {
        fs::path hp = HostPath(m_name);
        std::error_code ec;
        if (!fs::is_regular_file(hp, ec))
        {
            *err = FS_ENOENT;
            return false;
        }
        std::ifstream f(hp, std::ios::binary);
        m_data.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    }

    IOSReply Close() override
    {
        Flush();
        return { IPC_SUCCESS };
    }

    IOSReply Read(uint32_t addr, uint32_t size) override
    {
        if (!(m_mode & 1)) return { FS_EACCES };
        uint32_t n = (uint32_t)std::min<size_t>(size, m_data.size() > m_pos ? m_data.size() - m_pos : 0);
        memcpy(PhysPtr(addr), m_data.data() + m_pos, n);
        m_pos += n;
        return { (int32_t)n, false, 2000 + n / 4 };
    }

    IOSReply Write(uint32_t addr, uint32_t size) override
    {
        if (!(m_mode & 2)) return { FS_EACCES };
        if (m_pos + size > m_data.size()) m_data.resize(m_pos + size);
        memcpy(m_data.data() + m_pos, PhysPtr(addr), size);
        m_pos += size;
        m_dirty = true;
        return { (int32_t)size, false, 4000 + size / 2 };
    }

    IOSReply Seek(int32_t off, uint32_t whence) override
    {
        int64_t np;
        switch (whence)
        {
        case 0: np = off; break;
        case 1: np = (int64_t)m_pos + off; break;
        case 2: np = (int64_t)m_data.size() + off; break;
        default: return { FS_EINVAL };
        }
        if (np < 0 || np > (int64_t)m_data.size()) return { FS_EINVAL };
        m_pos = (size_t)np;
        return { (int32_t)m_pos };
    }

    IOSReply Ioctl(const IOSRequest& r) override
    {
        if (r.ioctl == 11)   // GetFileStats
        {
            Write32(0x80000000 | r.outAddr, (uint32_t)m_data.size());
            Write32(0x80000000 | (r.outAddr + 4), (uint32_t)m_pos);
            return { IPC_SUCCESS };
        }
        return IOSDevice::Ioctl(r);
    }

private:
    void Flush()
    {
        if (!m_dirty) return;
        fs::path hp = HostPath(m_name);
        std::ofstream f(hp, std::ios::binary | std::ios::trunc);
        f.write((const char*)m_data.data(), m_data.size());
        m_dirty = false;
    }

    uint32_t m_mode;
    std::vector<uint8_t> m_data;
    size_t m_pos = 0;
    bool m_dirty = false;
};

std::shared_ptr<IOSDevice> MakeNandFile(const std::string& path, uint32_t mode, int32_t* err)
{
    if (!ValidPath(path)) { *err = FS_EINVAL; return nullptr; }
    auto f = std::make_shared<NandFile>(path, mode);
    if (!f->Load(err)) return nullptr;
    *err = IPC_SUCCESS;
    return f;
}

// ------------------------------------------------------------------ /dev/fs
class FSDevice : public IOSDevice
{
public:
    FSDevice() : IOSDevice("/dev/fs") {}

    IOSReply Ioctl(const IOSRequest& r) override
    {
        uint32_t in = 0x80000000 | r.inAddr;
        uint32_t out = 0x80000000 | r.outAddr;
        switch (r.ioctl)
        {
        case 0x02:   // GetStats
        {
            memset(PhysPtr(r.outAddr), 0, r.outSize);
            if (r.outSize >= 0x1C)
            {
                Write32(out + 0x00, 0x4000);   // cluster size
                Write32(out + 0x04, 0x5DEC);   // free clusters
                Write32(out + 0x08, 0x1DD4);   // used clusters
                Write32(out + 0x0C, 0x10);
                Write32(out + 0x10, 0x10);
                Write32(out + 0x14, 0x146B);   // free inodes
                Write32(out + 0x18, 0x0394);   // used inodes
            }
            return { IPC_SUCCESS };
        }
        case 0x03:   // CreateDir
        case 0x09:   // CreateFile
        {
            std::string path = ReadGuestString(r.inAddr + 6);
            if (!ValidPath(path)) return { FS_EINVAL };
            fs::path hp = HostPath(path);
            std::error_code ec;
            if (fs::exists(hp, ec)) return { FS_EEXIST };
            if (!fs::exists(hp.parent_path(), ec)) return { FS_ENOENT };
            if (r.ioctl == 0x03)
                fs::create_directory(hp, ec);
            else
                std::ofstream(hp, std::ios::binary);
            LOG_INFO("FS: create {} {}", r.ioctl == 3 ? "dir" : "file", path);
            return { IPC_SUCCESS };
        }
        case 0x05:   // SetAttr
            return { IPC_SUCCESS };
        case 0x06:   // GetAttr
        {
            std::string path = ReadGuestString(r.inAddr);
            if (!ValidPath(path)) return { FS_EINVAL };
            std::error_code ec;
            if (!fs::exists(HostPath(path), ec)) return { FS_ENOENT };
            if (r.outSize >= 0x4C)
            {
                memset(PhysPtr(r.outAddr), 0, 0x4C);
                Write32(out, 0x1000);                 // owner uid
                Write16(out + 4, 0x3031);             // gid ("01")
                strncpy_s((char*)PhysPtr(r.outAddr + 6), 64, path.c_str(), 63);
                Write8(out + 0x46, 3);                // owner rw
                Write8(out + 0x47, 3);                // group rw
                Write8(out + 0x48, 3);                // other rw
                Write8(out + 0x49, 0);
            }
            return { IPC_SUCCESS };
        }
        case 0x07:   // Delete
        {
            std::string path = ReadGuestString(r.inAddr);
            if (!ValidPath(path)) return { FS_EINVAL };
            std::error_code ec;
            fs::path hp = HostPath(path);
            if (!fs::exists(hp, ec)) return { FS_ENOENT };
            fs::remove_all(hp, ec);
            LOG_INFO("FS: delete {}", path);
            return { IPC_SUCCESS };
        }
        case 0x08:   // Rename
        {
            std::string from = ReadGuestString(r.inAddr);
            std::string to = ReadGuestString(r.inAddr + 64);
            if (!ValidPath(from) || !ValidPath(to)) return { FS_EINVAL };
            std::error_code ec;
            fs::path a = HostPath(from), b = HostPath(to);
            if (!fs::exists(a, ec)) return { FS_ENOENT };
            if (fs::exists(b, ec)) fs::remove_all(b, ec);
            fs::rename(a, b, ec);
            LOG_INFO("FS: rename {} -> {}", from, to);
            return { ec ? FS_EINVAL : IPC_SUCCESS };
        }
        case 0x0A:   // SetFileVersionControl
            return { IPC_SUCCESS };
        case 0x0D:   // Shutdown
            return { IPC_SUCCESS };
        default:
            return IOSDevice::Ioctl(r);
        }
    }

    IOSReply Ioctlv(const IOSRequest& r) override
    {
        switch (r.ioctl)
        {
        case 0x04:   // ReadDir
        {
            if (r.in.empty()) return { FS_EINVAL };
            std::string path = ReadGuestString(r.in[0].addr);
            if (!ValidPath(path)) return { FS_EINVAL };
            fs::path hp = HostPath(path);
            std::error_code ec;
            if (!fs::is_directory(hp, ec)) return { FS_ENOENT };
            std::vector<std::string> names;
            for (auto& e : fs::directory_iterator(hp, ec))
                names.push_back(e.path().filename().string());
            if (r.in.size() == 1 && r.io.size() == 1)
            {
                Write32(0x80000000 | r.io[0].addr, (uint32_t)names.size());
                return { IPC_SUCCESS };
            }
            uint32_t maxEntries = Read32(0x80000000 | r.in[1].addr);
            uint8_t* o = PhysPtr(r.io[0].addr);
            memset(o, 0, r.io[0].size);
            uint32_t n = 0;
            size_t pos = 0;
            for (auto& nm : names)
            {
                if (n >= maxEntries) break;
                if (pos + nm.size() + 1 > r.io[0].size) break;
                memcpy(o + pos, nm.c_str(), nm.size() + 1);
                pos += nm.size() + 1;
                n++;
            }
            Write32(0x80000000 | r.io[1].addr, n);
            return { IPC_SUCCESS };
        }
        case 0x0C:   // GetUsage
        {
            if (r.io.size() >= 2)
            {
                Write32(0x80000000 | r.io[0].addr, 4);   // clusters
                Write32(0x80000000 | r.io[1].addr, 4);   // inodes
            }
            return { IPC_SUCCESS };
        }
        default:
            return IOSDevice::Ioctlv(r);
        }
    }
};

std::shared_ptr<IOSDevice> MakeFSDevice() { return std::make_shared<FSDevice>(); }

// ------------------------------------------------------------------ system files
struct SCEntry { uint8_t type; std::string name; std::vector<uint8_t> data; };

static std::vector<uint8_t> BuildSysconf()
{
    std::vector<SCEntry> e;
    e.push_back({ 1, "BT.DINF", std::vector<uint8_t>(0x460 + 1) });
    e.push_back({ 1, "BT.CDIF", std::vector<uint8_t>(0x204 + 1) });
    e.push_back({ 5, "BT.SENS", { 0, 0, 0, 3 } });
    e.push_back({ 3, "BT.BAR", { 1 } });
    e.push_back({ 3, "BT.SPKV", { 0x58 } });
    e.push_back({ 3, "BT.MOT", { 1 } });
    std::vector<uint8_t> nick = { 0, 'R', 0, 'e', 0, 'c', 0, 'o', 0, 'm', 0, 'p' };
    nick.resize(22);
    nick[21] = 6;
    e.push_back({ 2, "IPL.NIK", nick });
    e.push_back({ 3, "IPL.LNG", { (uint8_t)g_config.language } });
    std::vector<uint8_t> sadr(0x1007 + 1);
    sadr[0] = 0x6E;   // United Kingdom (PAL)
    e.push_back({ 1, "IPL.SADR", sadr });
    std::vector<uint8_t> pc(0x49 + 1);
    pc[1] = 0x04; pc[2] = 0x14;
    e.push_back({ 2, "IPL.PC", pc });
    e.push_back({ 5, "IPL.CB", { 0, 0, 0, 0 } });
    e.push_back({ 3, "IPL.AR", { (uint8_t)(g_config.widescreen ? 1 : 0) } });
    e.push_back({ 3, "IPL.SSV", { 1 } });
    e.push_back({ 7, "IPL.CD", { 0 } });
    e.push_back({ 7, "IPL.CD2", { 0 } });
    e.push_back({ 7, "IPL.EULA", { 1 } });
    e.push_back({ 3, "IPL.UPT", { 2 } });
    e.push_back({ 3, "IPL.PGS", { 1 } });   // progressive scan
    e.push_back({ 3, "IPL.E60", { 1 } });   // EuRGB60
    e.push_back({ 3, "IPL.DH", { 0 } });
    e.push_back({ 5, "IPL.INC", { 0, 0, 0, 8 } });
    e.push_back({ 5, "IPL.FRC", { 0, 0, 0, 0x28 } });
    e.push_back({ 2, "IPL.IDL", { 0, 1 } });
    e.push_back({ 5, "NET.WCFG", { 0, 0, 0, 1 } });
    e.push_back({ 5, "NET.CTPC", std::vector<uint8_t>(4) });
    e.push_back({ 3, "WWW.RST", { 0 } });
    e.push_back({ 7, "MPLS.MOVIE", { 1 } });

    std::vector<uint8_t> buf = { 'S', 'C', 'v', '0' };
    auto put16 = [](std::vector<uint8_t>& b, uint16_t v) { b.push_back(v >> 8); b.push_back((uint8_t)v); };
    put16(buf, (uint16_t)e.size());
    size_t entriesBegin = buf.size() + 2 * (e.size() + 1);
    std::vector<uint8_t> ent;
    for (auto& it : e)
    {
        put16(buf, (uint16_t)(entriesBegin + ent.size()));
        ent.push_back((uint8_t)((it.type << 5) | (it.name.size() - 1)));
        ent.insert(ent.end(), it.name.begin(), it.name.end());
        if (it.type == 1) put16(ent, (uint16_t)(it.data.size() - 1));
        else if (it.type == 2) ent.push_back((uint8_t)(it.data.size() - 1));
        ent.insert(ent.end(), it.data.begin(), it.data.end());
    }
    put16(buf, (uint16_t)(entriesBegin + ent.size()));
    buf.insert(buf.end(), ent.begin(), ent.end());
    buf.resize(0x4000);
    memcpy(buf.data() + 0x4000 - 4, "SCed", 4);
    return buf;
}

std::vector<uint8_t> BuildSettingTxt()
{
    std::vector<uint8_t> out(0x100, 0);
    uint32_t key = 0x73B5DBFA;
    size_t pos = 0;
    auto line = [&](const std::string& s) {
        for (char c : s)
        {
            if (pos >= out.size()) return;
            out[pos++] = (uint8_t)(c ^ (key & 0xFF));
            key = (key >> 31) | (key << 1);
        }
    };
    line("AREA=EUR\r\n");
    line("MODEL=RVL-001(EUR)\r\n");
    line("DVD=0\r\n");
    line("MPCH=0x7FFE\r\n");
    line("CODE=LEH\r\n");
    line("SERNO=123456789\r\n");
    line("VIDEO=PAL\r\n");
    line("GAME=EU\r\n");
    return out;
}

void NAND_InitSystemFiles()
{
    std::error_code ec;
    fs::create_directories(HostPath("/shared2/sys"), ec);
    fs::create_directories(HostPath("/title/00010000/52454e45/data"), ec);
    fs::create_directories(HostPath("/title/00000001/00000002/data"), ec);
    fs::create_directories(HostPath("/tmp"), ec);
    fs::create_directories(HostPath("/meta"), ec);
    fs::create_directories(HostPath("/sys"), ec);
    fs::path sc = HostPath("/shared2/sys/SYSCONF");
    {
        // regenerated every boot so config.ini changes (language, aspect) apply
        auto b = BuildSysconf();
        std::ofstream(sc, std::ios::binary).write((const char*)b.data(), b.size());
    }
    fs::path st = HostPath("/title/00000001/00000002/data/setting.txt");
    if (!fs::exists(st, ec))
    {
        auto b = BuildSettingTxt();
        std::ofstream(st, std::ios::binary).write((const char*)b.data(), b.size());
    }
}
