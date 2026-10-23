// bkr_mods_test: checks the mod overlay of the disc layer (runtime/mods, runtime/ios/disc.cpp)
// on a small synthetic disc in a temporary folder: load order, case-insensitive paths,
// disabled mods, mods for another release, files the disc does not have, larger and
// smaller replacements (FST offsets and sizes, partial reads) and the unmodded disc.
// Exit code 0 when every check passes.
#include "ios/disc.h"
#include "mods/mods.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// the runtime's log and TAS hooks
static std::vector<std::string> s_log;
void Log_Write(int level, const std::string& s)
{
    s_log.push_back(s);
    printf("  [%c] %s\n", "DIWE"[level & 3], s.c_str());
}
void Tas_OnFileRead(const std::string&) {}

static int s_failed = 0;
#define CHECK(cond)                                                     \
    do                                                                  \
    {                                                                   \
        if (cond) printf("PASS %s\n", #cond);                           \
        else { printf("FAIL %s (line %d)\n", #cond, __LINE__); s_failed++; } \
    } while (0)

static void Put32(std::vector<uint8_t>& v, size_t at, uint32_t x)
{
    v[at] = (uint8_t)(x >> 24); v[at + 1] = (uint8_t)(x >> 16); v[at + 2] = (uint8_t)(x >> 8); v[at + 3] = (uint8_t)x;
}
static uint32_t Get32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void WriteFile(const fs::path& p, const std::string& data)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(data.data(), data.size());
}

// FST entry of a file by name: offset, size
static bool FstFile(const std::vector<uint8_t>& fst, const char* name, uint64_t& off, uint32_t& size)
{
    uint32_t n = Get32(&fst[8]);
    const char* strings = (const char*)fst.data() + n * 12;
    for (uint32_t i = 1; i < n; i++)
    {
        const uint8_t* e = fst.data() + i * 12;
        if (e[0] == 0 && !strcmp(strings + (Get32(e) & 0xFFFFFF), name))
        {
            off = (uint64_t)Get32(e + 4) << 2;
            size = Get32(e + 8);
            return true;
        }
    }
    return false;
}

static std::string Read(uint64_t off, uint32_t len)
{
    std::string s(len, '\0');
    Disc_ReadPartition(off, len, (uint8_t*)s.data());
    return s;
}

static bool Logged(const std::string& part)
{
    for (auto& l : s_log)
        if (l.find(part) != std::string::npos) return true;
    return false;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    fs::path tmp = fs::temp_directory_path() / "bkr_mods_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::path data = tmp / "DATA", mods = tmp / "mods";

    // --- disc: a.bin (100), dir/b.bin (200), dir/c.one (50), d.bin (64)
    struct F { const char* name; int dir; uint32_t off; uint32_t size; char fill; };
    const F files[] = { { "a.bin", 0, 0x10000, 100, 'A' }, { "b.bin", 1, 0x10080, 200, 'B' },
                        { "c.one", 1, 0x10200, 50, 'C' }, { "d.bin", 0, 0x10280, 64, 'D' } };
    // entries: root, a.bin, dir (children 3..4), b.bin, c.one, d.bin
    std::string names;
    auto name = [&](const char* s) { uint32_t o = (uint32_t)names.size(); names += s; names += '\0'; return o; };
    std::vector<uint8_t> fst(6 * 12);
    fst[0] = 1; Put32(fst, 8, 6);
    auto fileEntry = [&](int idx, const F& f) { Put32(fst, idx * 12, name(f.name)); Put32(fst, idx * 12 + 4, f.off >> 2); Put32(fst, idx * 12 + 8, f.size); };
    fileEntry(1, files[0]);
    Put32(fst, 2 * 12, 0x01000000 | name("dir")); Put32(fst, 2 * 12 + 4, 0); Put32(fst, 2 * 12 + 8, 5);
    fileEntry(3, files[1]);
    fileEntry(4, files[2]);
    fileEntry(5, files[3]);
    fst.insert(fst.end(), names.begin(), names.end());
    std::vector<uint8_t> boot(0x440);
    memcpy(boot.data(), "RENE8P", 6);
    Put32(boot, 0x420, 0x8000 >> 2);   // main.dol
    Put32(boot, 0x424, 0x9000 >> 2);   // fst.bin
    Put32(boot, 0x428, (uint32_t)fst.size());
    WriteFile(data / "sys" / "boot.bin", std::string(boot.begin(), boot.end()));
    WriteFile(data / "sys" / "bi2.bin", std::string(0x2000, '\0'));
    WriteFile(data / "sys" / "apploader.img", std::string(0x100, 'L'));
    WriteFile(data / "sys" / "main.dol", std::string(0x200, 'M'));
    WriteFile(data / "sys" / "fst.bin", std::string(fst.begin(), fst.end()));
    for (auto& f : files) WriteFile(data / "files" / (f.dir ? "dir" : "") / f.name, std::string(f.size, f.fill));

    // --- mods
    // Alpha (top-level layout, mod.ini): a.bin larger, DIR/B.BIN smaller, an unknown file
    WriteFile(mods / "Alpha" / "mod.ini", "[Mod]\nName = Alpha Skin\nAuthor = Someone\nVersion = 1.2\n");
    WriteFile(mods / "Alpha" / "a.bin", std::string(300, 'a'));
    WriteFile(mods / "Alpha" / "DIR" / "B.BIN", std::string(10, 'b'));
    WriteFile(mods / "Alpha" / "notes" / "extra.dat", "x");
    // Beta (files/ layout): a.bin (loses to Alpha), dir/c.one same size
    WriteFile(mods / "Beta" / "files" / "a.bin", std::string(5, 'x'));
    WriteFile(mods / "Beta" / "files" / "dir" / "c.one", std::string(50, 'c'));
    // Gamma: disabled in the settings
    WriteFile(mods / "Gamma" / "d.bin", std::string(64, 'g'));
    // Delta: for the Europe release only
    WriteFile(mods / "Delta" / "mod.ini", "[Mod]\nTitle = Delta\nReleases = RENP8P\n");
    WriteFile(mods / "Delta" / "d.bin", std::string(64, 'e'));
    // Wrapped: an archive extracted with an extra folder level
    WriteFile(mods / "Wrapped" / "Inner" / "mod.ini", "Title=Wrapped Mod\n");
    WriteFile(mods / "Wrapped" / "Inner" / "d.bin", std::string(64, 'w'));

    std::vector<ModSetting> settings = { { "alpha", true }, { "Beta", true }, { "Gamma", false } };
    std::vector<ModInfo> list = Mods_Scan({ mods }, settings, "RENE8P");
    printf("scan:");
    for (auto& m : list) printf(" %s(%s%s)", m.folder.c_str(), m.enabled ? "on" : "off", m.forRelease ? "" : ", other release");
    printf("\n");
    CHECK(list.size() == 5);
    CHECK(list[0].folder == "Alpha" && list[1].folder == "Beta" && list[2].folder == "Gamma");
    CHECK(list[3].folder == "Delta" && list[4].folder == "Wrapped");   // new mods: by name, enabled
    CHECK(list[0].title == "Alpha Skin" && list[0].author == "Someone" && list[0].version == "1.2");
    CHECK(list[1].title == "Beta" && list[1].root == mods / "Beta" / "files");
    CHECK(!list[2].enabled && list[3].enabled && !list[3].forRelease);
    CHECK(list[4].title == "Wrapped Mod" && list[4].root == mods / "Wrapped" / "Inner");
    std::vector<ModSetting> saved = Mods_Settings(list);
    CHECK(saved.size() == 5 && saved[2].folder == "Gamma" && !saved[2].enabled);

    // --- overlay
    CHECK(Disc_Open(data.string(), "", list));
    const std::vector<uint8_t>& pf = Disc_Fst();
    uint64_t off; uint32_t size;
    CHECK(FstFile(pf, "a.bin", off, size) && size == 300 && off >= 0x18000 && off % 0x8000 == 0);
    CHECK(Read(off, 300) == std::string(300, 'a'));
    CHECK(Read(off + 123, 40) == std::string(40, 'a'));                       // partial read
    CHECK(Read(off + 290, 20) == std::string(10, 'a') + std::string(10, '\0'));   // past the end
    CHECK(Read(0x10000, 100) == std::string(100, '\0'));                      // old place is empty
    CHECK(FstFile(pf, "b.bin", off, size) && off == 0x10080 && size == 10);      // smaller: stays
    CHECK(Read(0x10080, 200) == std::string(10, 'b') + std::string(190, '\0'));
    CHECK(FstFile(pf, "c.one", off, size) && off == 0x10200 && size == 50);
    CHECK(Read(0x10200 + 7, 20) == std::string(20, 'c'));
    CHECK(FstFile(pf, "d.bin", off, size) && size == 64);
    CHECK(Read(off, 64) == std::string(64, 'w'));                              // Wrapped; Gamma off, Delta other release
    CHECK(Read(0x9000, (uint32_t)fst.size()) == std::string(pf.begin(), pf.end()));   // FST read from the disc
    CHECK(Read(0x8000, 4) == "MMMM");
    std::string whole;
    CHECK(Disc_ReadFileByPath("Dir/b.bin", whole) && whole == std::string(10, 'b'));
    CHECK(Logged("a.bin is replaced by more than one mod; Alpha Skin wins (load order) over Beta"));
    CHECK(Logged("Alpha Skin: 1 files are not game files (ignored): notes/extra.dat"));
    CHECK(Logged("Delta is not for this release"));
    CHECK(!Logged("Gamma"));

    // --- Beta first: it wins a.bin, which is smaller than the original and keeps its place
    std::swap(list[0], list[1]);
    s_log.clear();
    CHECK(Disc_Open(data.string(), "", list));
    CHECK(FstFile(Disc_Fst(), "a.bin", off, size) && off == 0x10000 && size == 5);
    CHECK(Read(0x10000, 8) == std::string(5, 'x') + std::string(3, '\0'));
    CHECK(Logged("a.bin is replaced by more than one mod; Beta wins (load order) over Alpha Skin"));

    // --- no mods: the installed files and the FST as installed
    CHECK(Disc_Open(data.string(), "", {}));
    CHECK(Disc_Fst() == fst);
    CHECK(Read(0x10000, 100) == std::string(100, 'A'));
    // across b.bin, the gap after it and c.one
    CHECK(Read(0x10080 + 50, 400) == std::string(150, 'B') + std::string(0x10200 - 0x10080 - 200, '\0') +
                                         std::string(50, 'C') + std::string(16, '\0'));
    CHECK(Read(0x10280, 64) == std::string(64, 'D'));
    CHECK(Disc_ReadFileByPath("dir/b.bin", whole) && whole == std::string(200, 'B'));

    // --- no mods folder at all
    CHECK(Mods_Scan({ tmp / "missing" }, settings, "RENE8P").empty());

    fs::remove_all(tmp, ec);   // best effort: the disc layer keeps its files open
    printf("%s (%d failed)\n", s_failed ? "FAILED" : "ALL PASSED", s_failed);
    return s_failed ? 1 : 0;
}
