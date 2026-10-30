#include "disc.h"
#include "../log.h"
#include "../mods/mods.h"
#include "../tas/tas.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

struct Extent
{
    uint64_t offset;   // byte offset inside the decrypted partition
    uint64_t size;
    std::filesystem::path host;   // empty: the patched FST in memory (s_fst)
    std::string path;             // host file as text (open-file cache key, TAS file conditions)
};

static std::vector<Extent> s_extents;   // sorted by offset
static std::vector<uint8_t> s_fst;      // fst.bin with the mods' files patched in
static std::map<std::string, std::filesystem::path> s_modFiles;   // files/ path, lower case -> mod file
static std::string s_dataDir;
static std::string s_rawPath;   // unencrypted disc area: disc/unencrypted.bin or the ISO
static uint8_t s_header[0x440];
static uint64_t s_partitionOffset = 0;
static std::mutex s_mutex;

// Small cache of open host files
static std::map<std::string, FILE*> s_files;

static FILE* GetFile(const std::string& p, const std::filesystem::path& host)
{
    auto it = s_files.find(p);
    if (it != s_files.end()) return it->second;
    // shared for deletion: the mod browser can replace or remove a loaded mod's files while
    // the game runs (it keeps reading the old ones until it restarts)
    FILE* f = nullptr;
#ifdef _WIN32
    HANDLE h = CreateFileW(host.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        int fd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_BINARY);
        if (fd < 0) CloseHandle(h);
        else if (!(f = _fdopen(fd, "rb"))) _close(fd);
    }
#else
    f = fopen(host.c_str(), "rb");   // POSIX files can always be replaced or removed while open
#endif
    if (s_files.size() > 64)
    {
        for (auto& kv : s_files) if (kv.second) fclose(kv.second);
        s_files.clear();
    }
    s_files[p] = f;
    return f;
}

static bool ReadWhole(const std::filesystem::path& p, std::vector<uint8_t>& out)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") || !f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n);
    fread(out.data(), 1, n, f);
    fclose(f);
    return true;
}

static inline uint32_t BE32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static void AddExtent(uint64_t off, uint64_t size, const std::filesystem::path& host, const std::string& path)
{
    if (size) s_extents.push_back({ off, size, host, path });
}

static void AddExtent(uint64_t off, uint64_t size, const std::string& path)
{
    AddExtent(off, size, std::filesystem::path(path), path);
}

static std::string Lower(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static void PutBE32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

bool Disc_Open(const std::string& dataDir, const std::string& isoPath, const std::vector<ModInfo>& mods)
{
    s_extents.clear();
    s_modFiles.clear();
    for (auto& kv : s_files) if (kv.second) fclose(kv.second);
    s_files.clear();
    s_dataDir = dataDir;
    // the installer saves the unencrypted area, so the ISO is only needed for older
    // DolphinTool extractions
    s_rawPath = dataDir + "/disc/unencrypted.bin";
    if (!std::filesystem::exists(s_rawPath)) s_rawPath = isoPath;
    std::string sys = dataDir + "/sys/";
    std::vector<uint8_t> boot, fst;
    if (!ReadWhole(sys + "boot.bin", boot) || boot.size() < 0x440)
    {
        LOG_ERROR("cannot read {}boot.bin", sys);
        return false;
    }
    memcpy(s_header, boot.data(), 0x440);
    if (!ReadWhole(sys + "fst.bin", fst))
    {
        LOG_ERROR("cannot read fst.bin");
        return false;
    }
    uint64_t dolOff = (uint64_t)BE32(&boot[0x420]) << 2;
    uint64_t fstOff = (uint64_t)BE32(&boot[0x424]) << 2;
    AddExtent(0, 0x440, sys + "boot.bin");
    AddExtent(0x440, 0x2000, sys + "bi2.bin");
    {
        std::vector<uint8_t> apl;
        ReadWhole(sys + "apploader.img", apl);
        AddExtent(0x2440, apl.size(), sys + "apploader.img");
    }
    {
        std::vector<uint8_t> dol;
        ReadWhole(sys + "main.dol", dol);
        AddExtent(dolOff, dol.size(), sys + "main.dol");
    }
    AddExtent(fstOff, fst.size(), std::filesystem::path(), "fst");

    // Walk the FST
    struct FstFile { uint32_t entry; std::string rel; uint64_t offset; uint32_t size; };
    std::vector<FstFile> files;
    uint32_t numEntries = BE32(&fst[8]);
    const uint8_t* strings = fst.data() + numEntries * 12;
    std::vector<std::pair<uint32_t, std::string>> dirStack;   // (end index, path relative to files/)
    dirStack.push_back({ numEntries, "" });
    for (uint32_t i = 1; i < numEntries; i++)
    {
        while (!dirStack.empty() && i >= dirStack.back().first)
            dirStack.pop_back();
        const uint8_t* e = fst.data() + i * 12;
        bool isDir = e[0] != 0;
        uint32_t nameOff = BE32(e) & 0xFFFFFF;
        std::string name((const char*)strings + nameOff);
        std::string rel = dirStack.back().second.empty() ? name : dirStack.back().second + "/" + name;
        if (isDir)
            dirStack.push_back({ BE32(e + 8), rel });
        else
            files.push_back({ i, rel, (uint64_t)BE32(e + 4) << 2, BE32(e + 8) });
    }

    // Mods: a replaced file is read from the mod. A file that is not larger keeps its place
    // on the disc; a larger one moves behind everything else. The FST the game reads (from
    // the disc and the copy in MEM1, Disc_Fst) gets the new offsets and sizes, so the sizes
    // the game sees and its reads always match the mod's file.
    std::vector<std::string> paths;
    for (auto& f : files) paths.push_back(f.rel);
    std::map<std::string, ModOverride> overlay = Mods_BuildOverlay(mods, paths);
    uint64_t end = 0;
    for (auto& x : s_extents) end = std::max(end, x.offset + x.size);
    for (auto& f : files) end = std::max(end, f.offset + f.size);
    auto align = [](uint64_t v) { return (v + 0x7FFF) & ~0x7FFFull; };
    end = align(end);
    for (auto& f : files)
    {
        auto o = overlay.find(f.rel);
        if (o == overlay.end())
        {
            AddExtent(f.offset, f.size, dataDir + "/files/" + f.rel);
            continue;
        }
        uint32_t size = (uint32_t)o->second.size;
        uint64_t off = f.offset;
        if (size > f.size)
        {
            off = end;
            end = align(end + size);
        }
        if ((off >> 2) > 0xFFFFFFFFull)
        {
            LOG_WARN("mods: no room for {} on the disc, using the original", f.rel);
            AddExtent(f.offset, f.size, dataDir + "/files/" + f.rel);
            continue;
        }
        auto u8 = o->second.host.u8string();
        AddExtent(off, size, o->second.host, std::string(u8.begin(), u8.end()));
        s_modFiles[Lower(f.rel)] = o->second.host;
        uint8_t* e = fst.data() + f.entry * 12;
        PutBE32(e + 4, (uint32_t)(off >> 2));
        PutBE32(e + 8, size);
    }
    s_fst = std::move(fst);
    std::sort(s_extents.begin(), s_extents.end(), [](const Extent& a, const Extent& b) { return a.offset < b.offset; });

    // Find the data partition offset (partition table at 0x40000)
    FILE* iso = nullptr;
    if (!fopen_s(&iso, s_rawPath.c_str(), "rb") && iso)
    {
        uint8_t pt[0x20];
        _fseeki64(iso, 0x40000, SEEK_SET);
        fread(pt, 1, sizeof(pt), iso);
        uint32_t count = BE32(pt);
        uint64_t tableOff = (uint64_t)BE32(pt + 4) << 2;
        std::vector<uint8_t> tab(count * 8);
        _fseeki64(iso, tableOff, SEEK_SET);
        fread(tab.data(), 1, tab.size(), iso);
        for (uint32_t i = 0; i < count; i++)
        {
            if (BE32(&tab[i * 8 + 4]) == 0)
                s_partitionOffset = (uint64_t)BE32(&tab[i * 8]) << 2;
        }
        fclose(iso);
    }
    else
    {
        LOG_WARN("no disc/unencrypted.bin and ISO {} not available; raw disc reads will return zeros", isoPath);
    }
    LOG_INFO("disc: {} extents ({} from mods), game id {:.6}, data partition at {:x}", s_extents.size(), s_modFiles.size(),
             (const char*)s_header, s_partitionOffset);
    return true;
}

bool Disc_ReadPartition(uint64_t offset, uint32_t length, uint8_t* out)
{
    std::lock_guard<std::mutex> lk(s_mutex);
    memset(out, 0, length);
    uint64_t end = offset + length;
    // first extent that might overlap
    auto it = std::upper_bound(s_extents.begin(), s_extents.end(), offset, [](uint64_t v, const Extent& e) { return v < e.offset; });
    if (it != s_extents.begin()) --it;
    bool any = false;
    for (; it != s_extents.end() && it->offset < end; ++it)
    {
        uint64_t s = std::max(offset, it->offset);
        uint64_t e = std::min(end, it->offset + it->size);
        if (s >= e) continue;
        if (it->host.empty())
        {
            memcpy(out + (s - offset), s_fst.data() + (s - it->offset), (size_t)(e - s));
            any = true;
            continue;
        }
        FILE* f = GetFile(it->path, it->host);
        if (!f) continue;
        if (s == it->offset) Tas_OnFileRead(it->path);
        _fseeki64(f, (int64_t)(s - it->offset), SEEK_SET);
        fread(out + (s - offset), 1, (size_t)(e - s), f);
        any = true;
    }
    if (!any)
        LOG_WARN("disc read {:x}+{:x} hits no file", offset, length);
    return true;
}

bool Disc_ReadRaw(uint64_t offset, uint32_t length, uint8_t* out)
{
    std::lock_guard<std::mutex> lk(s_mutex);
    memset(out, 0, length);
    FILE* f = GetFile(s_rawPath, std::filesystem::path(s_rawPath));
    if (!f) return false;
    _fseeki64(f, (int64_t)offset, SEEK_SET);
    fread(out, 1, length, f);
    return true;
}

const uint8_t* Disc_Header() { return s_header; }
const std::string& Disc_DataDir() { return s_dataDir; }
uint64_t Disc_PartitionOffset() { return s_partitionOffset; }
const std::vector<uint8_t>& Disc_Fst() { return s_fst; }

bool Disc_ReadFileByPath(const std::string& discPath, std::string& out)
{
    std::vector<uint8_t> v;
    auto mod = s_modFiles.find(Lower(discPath));
    if (!ReadWhole(mod != s_modFiles.end() ? mod->second : std::filesystem::path(s_dataDir + "/files/" + discPath), v))
        return false;
    out.assign((const char*)v.data(), v.size());
    return true;
}
