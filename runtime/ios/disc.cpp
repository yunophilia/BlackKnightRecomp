#include "disc.h"
#include "../log.h"
#include "../tas/tas.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

struct Extent
{
    uint64_t offset;   // byte offset inside the decrypted partition
    uint64_t size;
    std::string path;  // host file
};

static std::vector<Extent> s_extents;   // sorted by offset
static std::string s_dataDir;
static std::string s_isoPath;
static uint8_t s_header[0x440];
static uint64_t s_partitionOffset = 0;
static std::mutex s_mutex;

// Small cache of open host files
static std::map<std::string, FILE*> s_files;

static FILE* GetFile(const std::string& p)
{
    auto it = s_files.find(p);
    if (it != s_files.end()) return it->second;
    FILE* f = nullptr;
    fopen_s(&f, p.c_str(), "rb");
    if (s_files.size() > 64)
    {
        for (auto& kv : s_files) if (kv.second) fclose(kv.second);
        s_files.clear();
    }
    s_files[p] = f;
    return f;
}

static bool ReadWhole(const std::string& p, std::vector<uint8_t>& out)
{
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "rb") || !f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n);
    fread(out.data(), 1, n, f);
    fclose(f);
    return true;
}

static inline uint32_t BE32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static void AddExtent(uint64_t off, uint64_t size, const std::string& path)
{
    if (size) s_extents.push_back({ off, size, path });
}

bool Disc_Open(const std::string& dataDir, const std::string& isoPath)
{
    s_dataDir = dataDir;
    s_isoPath = isoPath;
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
    AddExtent(fstOff, fst.size(), sys + "fst.bin");

    // Walk the FST
    uint32_t numEntries = BE32(&fst[8]);
    const uint8_t* strings = fst.data() + numEntries * 12;
    std::vector<std::pair<uint32_t, std::string>> dirStack;   // (end index, path)
    std::string cur = dataDir + "/files";
    dirStack.push_back({ numEntries, cur });
    for (uint32_t i = 1; i < numEntries; i++)
    {
        while (!dirStack.empty() && i >= dirStack.back().first)
            dirStack.pop_back();
        const uint8_t* e = fst.data() + i * 12;
        bool isDir = e[0] != 0;
        uint32_t nameOff = BE32(e) & 0xFFFFFF;
        std::string name((const char*)strings + nameOff);
        std::string full = dirStack.back().second + "/" + name;
        if (isDir)
        {
            dirStack.push_back({ BE32(e + 8), full });
        }
        else
        {
            uint64_t off = (uint64_t)BE32(e + 4) << 2;
            uint32_t size = BE32(e + 8);
            AddExtent(off, size, full);
        }
    }
    std::sort(s_extents.begin(), s_extents.end(), [](const Extent& a, const Extent& b) { return a.offset < b.offset; });

    // Find the data partition offset in the raw ISO (partition table at 0x40000)
    FILE* iso = nullptr;
    if (!fopen_s(&iso, isoPath.c_str(), "rb") && iso)
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
        LOG_WARN("ISO {} not available; raw disc reads will return zeros", isoPath);
    }
    LOG_INFO("disc: {} extents, game id {:.6}, data partition at {:x}", s_extents.size(), (const char*)s_header, s_partitionOffset);
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
        FILE* f = GetFile(it->path);
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
    FILE* f = GetFile(s_isoPath);
    if (!f) return false;
    _fseeki64(f, (int64_t)offset, SEEK_SET);
    fread(out, 1, length, f);
    return true;
}

const uint8_t* Disc_Header() { return s_header; }
const std::string& Disc_DataDir() { return s_dataDir; }
uint64_t Disc_PartitionOffset() { return s_partitionOffset; }

bool Disc_ReadFileByPath(const std::string& discPath, std::string& out)
{
    std::vector<uint8_t> v;
    if (!ReadWhole(s_dataDir + "/files/" + discPath, v)) return false;
    out.assign((const char*)v.data(), v.size());
    return true;
}
