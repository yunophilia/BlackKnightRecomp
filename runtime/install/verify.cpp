#include "verify.h"
#include "releases.h"
#include "../platform/sha1.h"

#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

namespace fs = std::filesystem;

namespace
{
std::string Hex(const uint8_t* d, size_t n)
{
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += digits[d[i] >> 4]; s += digits[d[i] & 15]; }
    return s;
}

// SHA-1 of a file; false if it cannot be read (or the check was canceled)
bool HashFile(const fs::path& p, std::string& hex, VerifyProgress* progress)
{
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    Sha1 h;
    if (!h.Ok()) { fclose(f); return false; }
    std::vector<uint8_t> buf(1 << 20);
    bool ok = true;
    for (;;)
    {
        size_t n = fread(buf.data(), 1, buf.size(), f);
        if (n == 0) break;
        h.Update(buf.data(), n);
        if (progress)
        {
            progress->done += n;
            if (progress->cancel) { ok = false; break; }
        }
    }
    uint8_t out[20];
    h.Final(out);
    fclose(f);
    hex = Hex(out, 20);
    return ok;
}

const KnownFile* Find(const GameRelease& rel, const char* path)
{
    for (auto& k : std::span(rel.files, rel.fileCount))
        if (strcmp(k.path, path) == 0) return &k;
    return nullptr;
}

// the release installed in data, or null with the reason
const GameRelease* Installed(const fs::path& data, std::string& problem)
{
    std::string id = Release_InstalledId(data);
    const GameRelease* rel = Release_Find(id);
    if (!rel)
        problem = id.empty() ? "sys/boot.bin is missing"
                             : "the installed game (" + id + ") is not Sonic and the Black Knight (" + kReleaseNames + ")";
    return rel;
}
}   // namespace

DataStatus Install_QuickCheck(const fs::path& data, std::string& problem)
{
    std::error_code ec;
    if (!fs::exists(data / "sys" / "main.dol", ec))
    {
        problem = "the game data is not installed";
        return DataStatus::Missing;
    }
    const GameRelease* rel = Installed(data, problem);
    if (!rel) return DataStatus::Corrupt;
    int bad = 0;
    for (auto& k : std::span(rel->files, rel->fileCount))
    {
        uint64_t size = fs::file_size(data / fs::path(std::u8string((const char8_t*)k.path)), ec);
        if (ec || size != k.size)
        {
            if (bad++ == 0) problem = std::string(k.path) + (ec ? " is missing" : " has the wrong size");
        }
    }
    if (bad > 1) problem += " (and " + std::to_string(bad - 1) + " more)";
    if (bad) return DataStatus::Corrupt;
    // the recompiled code only matches this exact main.dol
    std::string hex;
    const KnownFile* dol = Find(*rel, "sys/main.dol");
    if (!HashFile(data / "sys" / "main.dol", hex, nullptr) || !dol || hex != dol->sha1)
    {
        problem = std::string("sys/main.dol does not match the supported ") + rel->name + " release";
        return DataStatus::Corrupt;
    }
    return DataStatus::Ok;
}

DataStatus Install_FullCheck(const fs::path& data, VerifyProgress& progress, std::string& problem)
{
    DataStatus quick = Install_QuickCheck(data, problem);
    if (quick != DataStatus::Ok) return quick;
    const GameRelease* rel = Installed(data, problem);
    auto files = std::span(rel->files, rel->fileCount);
    uint64_t total = 0;
    for (auto& k : files) total += k.size;
    progress.total = total;
    progress.done = 0;
    int bad = 0;
    for (auto& k : files)
    {
        std::string hex;
        if (!HashFile(data / fs::path(std::u8string((const char8_t*)k.path)), hex, &progress))
        {
            if (progress.cancel) break;
            hex.clear();
        }
        if (hex != k.sha1 && bad++ == 0) problem = std::string(k.path) + " is damaged";
    }
    if (bad > 1) problem += " (and " + std::to_string(bad - 1) + " more)";
    return bad ? DataStatus::Corrupt : DataStatus::Ok;
}
