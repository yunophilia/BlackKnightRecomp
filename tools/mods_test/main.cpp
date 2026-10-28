// bkr_mods_test: checks the mods code (runtime/mods, runtime/ios/disc.cpp) in temporary
// folders, without the game or the network:
//  * the disc overlay on a small synthetic disc: load order, case-insensitive paths, disabled
//    mods, mods for another release, files the disc does not have, larger and smaller
//    replacements (FST offsets and sizes, partial reads) and the unmodded disc;
//  * installing archives (zip and 7z written here, RAR from data/): mod folders, files/ layouts, texture packs,
//    variants, code mods and Dolphin-only archives (refused), unsafe paths, updates, removal;
//  * hostile input: injected or edited install records (gamebanana.json, the old .ini), device names, other formats,
//    entry counts, plain-http downloads;
//  * reading GameBanana's API answers.
// Exit code 0 when every check passes.
//
// bkr_mods_test --install <archive|gamebanana:<mod id>> --data <game data> --mods <dir>
//               --textures <dir>
// installs a real archive (or a mod's newest GameBanana file) with the same code as the
// game's mod browser and prints the result.
#include "ios/disc.h"
#include "mods/gamebanana.h"
#include "mods/mod_install.h"
#include "mods/mods.h"
#include "net/http.h"

#include <nlohmann/json.hpp>

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
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
extern const char g_version[] = "test";

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

static void OverlayTests()
{
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
    // the browser may replace a loaded mod while the game runs: its open files can move
    {
        std::error_code rec;
        fs::rename(mods / "Wrapped" / "Inner" / "d.bin", mods / "Wrapped" / "Inner" / "d.old", rec);
        CHECK(!rec);
        fs::rename(mods / "Wrapped" / "Inner" / "d.old", mods / "Wrapped" / "Inner" / "d.bin", rec);
    }

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
}

// --- archives for the install tests, written with libarchive
struct Entry { std::string path, data; };
static bool WriteArchive(const fs::path& file, bool sevenZip, const std::vector<Entry>& entries)
{
    archive* a = archive_write_new();
    if (sevenZip)
    {
        archive_write_set_format_7zip(a);
        archive_write_set_format_option(a, "7zip", "compression", "lzma2");
    }
    else
    {
        archive_write_set_format_zip(a);
        archive_write_set_format_option(a, "zip", "compression", "deflate");
    }
    if (archive_write_open_filename_w(a, file.c_str()) != ARCHIVE_OK) { archive_write_free(a); return false; }
    for (auto& e : entries)
    {
        archive_entry* en = archive_entry_new();
        archive_entry_set_pathname_utf8(en, e.path.c_str());
        archive_entry_set_filetype(en, AE_IFREG);
        archive_entry_set_perm(en, 0644);
        archive_entry_set_size(en, (la_int64_t)e.data.size());
        archive_write_header(a, en);
        archive_write_data(a, e.data.data(), e.data.size());
        archive_entry_free(en);
    }
    archive_write_close(a);
    archive_write_free(a);
    return true;
}

static std::string ReadText(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void InstallTests()
{
    fs::path tmp = fs::temp_directory_path() / "bkr_mods_install_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "archives");
    InstallTargets t;
    t.modsDir = tmp / "mods";
    t.texturesDir = tmp / "textures";
    t.gameId = "RENE8P";
    t.discFiles = { "a.bin", "dir/b.bin", "dir/c.one", "d.bin", "sonic.one", "adx/sng_stg11.adx" };
    t.recycle = false;
    auto src = [](uint64_t id, const char* name) {
        InstallSource s;
        s.modId = id;
        s.fileId = id * 10;
        s.name = name;
        s.author = "Tester";
        s.version = "1.0";
        s.fileName = "x.zip";
        s.fileAdded = 1000;
        return s;
    };
    fs::path arc = tmp / "archives";

    // a Storybook-style mod: folder with mod.ini, an Additional Files variant, a readme beside it
    WriteArchive(arc / "skin.zip", false, { { "Cool Skin/mod.ini", "[Mod]\nname = Cool Skin\nauthor = Someone\n" },
                                            { "Cool Skin/sonic.one", "SKIN" },
                                            { "Cool Skin/Additional Files/sonic.one", "ALT" },
                                            { "Cool Skin/Pictures/shot.jpg", "jpg" },
                                            { "TUT INSTALL.txt", "copy it" } });
    InstallResult r = ModInstall_Archive(arc / "skin.zip", src(1, "Cool Skin (GB)"), t);
    printf("  -> %s\n", r.message.c_str());
    CHECK(r.ok && r.modFolders.size() == 1 && r.modFolders[0] == "Cool Skin" && r.textureFolders.empty());
    CHECK(ReadText(t.modsDir / "Cool Skin" / "sonic.one") == "SKIN");
    CHECK(fs::exists(t.modsDir / "Cool Skin" / "Additional Files" / "sonic.one"));
    CHECK(ReadText(t.modsDir / "Cool Skin" / "mod.ini").find("Someone") != std::string::npos);   // kept
    CHECK(!fs::exists(t.modsDir / "TUT INSTALL.txt"));

    // 7z, the archive's top level is the mirror: folder named after the mod, mod.ini written
    WriteArchive(arc / "hud.7z", true, { { "dir/b.bin", "HUD" }, { "d.bin", "D" }, { "readme.txt", "r" } });
    r = ModInstall_Archive(arc / "hud.7z", src(2, "No HUD: Test?"), t);
    printf("  -> %s\n", r.message.c_str());
    CHECK(r.ok && r.modFolders.size() == 1 && r.modFolders[0] == "No HUD_ Test_");
    CHECK(ReadText(t.modsDir / "No HUD_ Test_" / "dir" / "b.bin") == "HUD");
    std::string ini = ReadText(t.modsDir / "No HUD_ Test_" / "mod.ini");
    CHECK(ini.find("name = No HUD: Test?") != std::string::npos && ini.find("author = Tester") != std::string::npos);

    // files/ layout below a generic folder name
    WriteArchive(arc / "files.zip", false, { { "DATA/files/adx/sng_stg11.adx", "MUSIC" }, { "DATA/mod.ini", "title=Music\n" } });
    r = ModInstall_Archive(arc / "files.zip", src(3, "Music Mod"), t);
    CHECK(r.ok && r.modFolders.size() == 1 && r.modFolders[0] == "Music Mod");
    CHECK(ReadText(t.modsDir / "Music Mod" / "files" / "adx" / "sng_stg11.adx") == "MUSIC");
    CHECK(ReadText(t.modsDir / "Music Mod" / "mod.ini") == "title=Music\n");

    // Dolphin texture pack (game ID folder) with Dolphin settings next to it
    WriteArchive(arc / "tex.zip", false, { { "Load/Textures/RENE8P/hud/tex1_64x64_0123456789abcdef_5.png", "PNG" },
                                           { "Load/Textures/RENE8P/tex1_32x32_fedcba9876543210_14.dds", "DDS" },
                                           { "Config/Profiles/Wiimote/Xbox.ini", "[Profile]\nButtons/A = `Button A`\n" } });
    r = ModInstall_Archive(arc / "tex.zip", src(4, "HD Pack"), t);
    printf("  -> %s\n", r.message.c_str());
    CHECK(r.ok && r.modFolders.empty() && r.textureFolders.size() == 1 && r.textureFolders[0] == "RENE8P/HD Pack");
    CHECK(fs::exists(t.texturesDir / "RENE8P" / "HD Pack" / "hud" / "tex1_64x64_0123456789abcdef_5.png"));
    CHECK(fs::exists(t.texturesDir / "RENE8P" / "HD Pack" / "tex1_32x32_fedcba9876543210_14.dds"));
    CHECK(r.notes.size() == 1 && r.notes[0].find("Dolphin settings") != std::string::npos);

    // a texture pack without a game ID folder goes to the installed release's folder
    WriteArchive(arc / "tex2.zip", false, { { "pack/tex1_8x8_00000000000000aa_3.png", "P" } });
    // nested game ID folders: the innermost one is the pack
    WriteArchive(arc / "tex3.zip", false, { { "rene8p/RENE8P/tex1_8x8_00000000000000bb_3.png", "P" } });
    r = ModInstall_Archive(arc / "tex3.zip", src(13, "Nested"), t);
    CHECK(r.ok && fs::exists(t.texturesDir / "RENE8P" / "Nested" / "tex1_8x8_00000000000000bb_3.png"));
    r = ModInstall_Archive(arc / "tex2.zip", src(5, "Small Pack"), t);
    CHECK(r.ok && r.textureFolders.size() == 1 && r.textureFolders[0] == "RENE8P/Small Pack");

    // code mods are refused, Dolphin-only archives too
    WriteArchive(arc / "code.zip", false, { { "Knights/sys/main.dol", "DOL" }, { "Knights/a.bin", "A" } });
    r = ModInstall_Archive(arc / "code.zip", src(6, "Knights"), t);
    printf("  -> %s\n", r.message.c_str());
    CHECK(!r.ok && r.message.find("code") != std::string::npos && !fs::exists(t.modsDir / "Knights"));
    WriteArchive(arc / "gecko.zip", false, { { "GameSettings/RENE8P.ini", "[Gecko]\n$Moon jump\n" } });
    r = ModInstall_Archive(arc / "gecko.zip", src(7, "Codes"), t);
    printf("  -> %s\n", r.message.c_str());
    CHECK(!r.ok && r.message.find("Dolphin settings or codes") != std::string::npos);
    WriteArchive(arc / "nothing.zip", false, { { "readme.txt", "hello" } });
    r = ModInstall_Archive(arc / "nothing.zip", src(8, "Nothing"), t);
    CHECK(!r.ok && r.message.find("no game files") != std::string::npos);
    { std::ofstream(arc / "broken.rar", std::ios::binary) << "Rar!garbage"; }
    r = ModInstall_Archive(arc / "broken.rar", src(9, "Broken"), t);
    printf("  -> %s\n", r.message.c_str());
    CHECK(!r.ok && r.message.find("archive") != std::string::npos);

    // paths past Windows' 260 characters
    std::string deep = "Deep/" + std::string(120, 'x') + "/" + std::string(90, 'y') + "/Long Mod";
    WriteArchive(arc / "deep.zip", false, { { deep + "/dir/c.one", "LONG" }, { deep + "/Additional Files/" + std::string(60, 'z') + ".one", "Z" } });
    r = ModInstall_Archive(arc / "deep.zip", src(14, "Deep"), t);
    printf("  -> %s\n", r.message.c_str());
    CHECK(r.ok && r.modFolders.size() == 1 && r.modFolders[0] == "Long Mod" && ReadText(t.modsDir / "Long Mod" / "dir" / "c.one") == "LONG");

    // variants: two folders, each a mod
    WriteArchive(arc / "variants.zip", false, { { "Version A/a.bin", "A1" }, { "Version B/a.bin", "B1" } });
    r = ModInstall_Archive(arc / "variants.zip", src(10, "Variants"), t);
    CHECK(r.ok && r.modFolders.size() == 2 && r.modFolders[0] == "Version A" && r.modFolders[1] == "Version B");

    // paths leaving the folder are not unpacked
    WriteArchive(arc / "evil.zip", false, { { "../evil.bin", "E" }, { "Evil/a.bin", "A" }, { "C:/evil2.bin", "E" } });
    r = ModInstall_Archive(arc / "evil.zip", src(11, "Evil"), t);
    CHECK(r.ok && !fs::exists(tmp / "evil.bin") && !fs::exists(t.modsDir / "evil.bin"));

    // a folder copied by hand with the same name stays; the install takes another name
    WriteFile(t.modsDir / "Hand Made" / "a.bin", "MINE");
    WriteArchive(arc / "hand.zip", false, { { "Hand Made/a.bin", "GB" } });
    r = ModInstall_Archive(arc / "hand.zip", src(12, "Hand Made"), t);
    CHECK(r.ok && r.modFolders.size() == 1 && r.modFolders[0] == "Hand Made (2)");
    CHECK(ReadText(t.modsDir / "Hand Made" / "a.bin") == "MINE");

    // update: same GameBanana mod, new file: same folder, new content
    WriteArchive(arc / "skin2.zip", false, { { "Cool Skin v2/sonic.one", "SKIN2" } });
    InstallSource s2 = src(1, "Cool Skin (GB)");
    s2.fileId = 11;
    s2.fileAdded = 2000;
    r = ModInstall_Archive(arc / "skin2.zip", s2, t);
    CHECK(r.ok && r.modFolders.size() == 1 && r.modFolders[0] == "Cool Skin");
    CHECK(ReadText(t.modsDir / "Cool Skin" / "sonic.one") == "SKIN2" && !fs::exists(t.modsDir / "Cool Skin v2"));

    // records: the installed mods, once each, not the refused ones
    std::vector<InstalledMod> recs = ModInstall_Records(t.modsDir);
    auto rec = [&](uint64_t id) -> const InstalledMod* {
        for (auto& x : recs)
            if (x.modId == id) return &x;
        return nullptr;
    };
    CHECK(recs.size() == 10);   // 1 2 3 4 5 10 11 12 13 14 (1 only once)
    CHECK(rec(1) && rec(1)->fileId == 11 && rec(1)->fileAdded == 2000 && rec(1)->mods.size() == 1);
    CHECK(rec(4) && rec(4)->textures.size() == 1 && rec(4)->mods.empty());
    CHECK(!rec(6) && !rec(7) && !rec(8) && !rec(9));

    // what the game makes of it
    std::vector<ModInfo> list = Mods_Scan({ t.modsDir }, {}, "RENE8P");
    bool sawMusic = false, sawStaging = false;
    for (auto& m : list)
    {
        sawMusic |= m.folder == "Music Mod" && m.title == "Music" && m.root == t.modsDir / "Music Mod" / "files";
        sawStaging |= m.folder[0] == '.';
    }
    CHECK(sawMusic && !sawStaging);
    std::map<std::string, ModOverride> ov = Mods_BuildOverlay(list, t.discFiles);
    CHECK(ov.count("sonic.one") && ReadText(ov["sonic.one"].host) == "SKIN2");
    CHECK(ov.count("adx/sng_stg11.adx") && ov.count("dir/b.bin"));

    // removal
    std::string msg;
    CHECK(ModInstall_Remove(t, 4, msg) && !fs::exists(t.texturesDir / "RENE8P" / "HD Pack"));
    CHECK(ModInstall_RemoveFolder(t, "Cool Skin", msg) && !fs::exists(t.modsDir / "Cool Skin"));
    CHECK(!ModInstall_RemoveFolder(t, "", msg) && !ModInstall_RemoveFolder(t, "..", msg) && fs::exists(t.modsDir));
    recs = ModInstall_Records(t.modsDir);
    CHECK(recs.size() == 8 && !rec(1) && !rec(4));
    CHECK(fs::exists(t.modsDir / "Hand Made" / "a.bin"));   // untouched
    CHECK(!fs::exists(t.modsDir / ".staging"));

    fs::remove_all(tmp, ec);
}

// --- what a hostile archive, API answer or edited install record can do
static void HardeningTests()
{
    fs::path tmp = fs::temp_directory_path() / "bkr_mods_hardening_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "archives");
    InstallTargets t;
    t.modsDir = tmp / "mods";
    t.texturesDir = tmp / "textures";
    t.gameId = "RENE8P";
    t.discFiles = { "a.bin", "dir/b.bin", "test.txt", "testdir/test.txt", "test.bin" };
    t.recycle = false;
    auto src = [](uint64_t id, const std::string& name) {
        InstallSource s;
        s.modId = id;
        s.fileId = id * 10;
        s.name = name;
        s.author = "Tester";
        s.version = "1.0";
        s.fileName = "x.zip";
        return s;
    };
    fs::path arc = tmp / "archives";
    WriteFile(tmp / "outside" / "keep.txt", "KEEP");
    WriteFile(tmp / "textures" / "keep.txt", "KEEP");

    // text from GameBanana with line breaks, quotes and paths stays plain text
    WriteArchive(arc / "inject.zip", false, { { "Inject/a.bin", "A" } });
    InstallSource s = src(20, "Inject\n[1]\nmod = ..\\..");
    s.version = "1\n[1]\nmod = ..\\..\"C:/x";
    s.fileName = "x\".zip\r\n\"mods\": [\"../outside\"]";
    s.author = "Me\nversion = 99";
    InstallResult r = ModInstall_Archive(arc / "inject.zip", s, t);
    CHECK(r.ok && r.modFolders.size() == 1 && r.modFolders[0] == "Inject");
    std::vector<InstalledMod> recs = ModInstall_Records(t.modsDir);
    CHECK(recs.size() == 1 && recs[0].name == s.name && recs[0].version == s.version && recs[0].fileName == s.fileName);
    CHECK(recs[0].mods.size() == 1 && recs[0].mods[0] == "Inject" && recs[0].textures.empty());
    CHECK(!fs::exists(t.modsDir / "gamebanana.ini") && fs::exists(t.modsDir / "gamebanana.json"));
    CHECK(ReadText(t.modsDir / "Inject" / "mod.ini").find("\nversion = 99") == std::string::npos);

    // a hand-edited file: folders that are not right below mods/ or textures/<ID>/, and
    // wrong types, are ignored...
    fs::path recFile = t.modsDir / "gamebanana.json";
    nlohmann::json doc = nlohmann::json::parse(ReadText(recFile));
    doc["records"].push_back({ { "id", 30 }, { "name", "Thirty" }, { "file", 300 },
                               { "mods", { "../outside", (tmp / "outside").string(), "Inject/..", "a/b", "CON", ".", "..",
                                           "C:/x", 5, nullptr, nlohmann::json::array({ "Inject" }) } },
                               { "textures", { "RENE8P/../../outside", "..\\outside", "RENE8P", "RENE8P/Pack", "C:/x/y" } } });
    doc["records"].push_back({ { "id", "31" }, { "mods", nlohmann::json::array({ "Inject" }) } });          // id of the wrong type
    doc["records"].push_back({ { "id", 32 }, { "mods", "Inject" } });                 // not a list
    doc["records"].push_back({ { "id", 33 }, { "mods", nlohmann::json::array({ "../outside" }) }, { "textures", nlohmann::json::array({ "RENE8P/.." }) } });
    doc["records"].push_back("not a record");
    WriteFile(recFile, doc.dump(2));
    recs = ModInstall_Records(t.modsDir);
    auto find = [&](uint64_t id) -> const InstalledMod* {
        for (auto& x : recs)
            if (x.modId == id) return &x;
        return nullptr;
    };
    CHECK(find(20) && find(30) && !find(31) && find(32) && find(33) && recs.size() == 4);
    CHECK(find(30)->mods.empty() && find(30)->textures.size() == 1 && find(30)->textures[0] == "RENE8P/Pack");
    CHECK(find(32)->mods.empty() && find(33)->mods.empty() && find(33)->textures.empty());
    // ...by an update (which replaces the record's folders)...
    WriteArchive(arc / "thirty.zip", false, { { "Thirty/a.bin", "30" }, { "RENE8P/tex1_8x8_00000000000000cc_3.png", "P" } });
    r = ModInstall_Archive(arc / "thirty.zip", src(30, "Thirty"), t);
    CHECK(r.ok && r.modFolders.size() == 1 && r.modFolders[0] == "Thirty" && r.textureFolders.size() == 1 &&
          r.textureFolders[0] == "RENE8P/Pack");
    CHECK(ReadText(tmp / "outside" / "keep.txt") == "KEEP" && ReadText(tmp / "textures" / "keep.txt") == "KEEP");
    // ...and by Remove
    std::string msg;
    ModInstall_Remove(t, 33, msg);
    ModInstall_Remove(t, 32, msg);
    CHECK(ModInstall_Remove(t, 30, msg) && !fs::exists(t.modsDir / "Thirty") && !fs::exists(t.texturesDir / "RENE8P" / "Pack"));
    CHECK(ReadText(tmp / "outside" / "keep.txt") == "KEEP" && ReadText(tmp / "textures" / "keep.txt") == "KEEP");
    CHECK(fs::exists(t.modsDir / "Inject" / "a.bin"));
    recs = ModInstall_Records(t.modsDir);
    CHECK(recs.size() == 1 && find(20));

    // a broken file is ignored (and replaced by the next install)
    std::string good = ReadText(recFile);
    for (const char* bad : { "{ \"records\": [ {\"id\": 1, ", "[1, 2, 3]", "{\"records\": 7}", "" })
    {
        WriteFile(recFile, bad);
        CHECK(ModInstall_Records(t.modsDir).empty());
    }
    WriteFile(recFile, good);

    // gamebanana.ini from the first versions: moved to gamebanana.json once, same checks
    InstallTargets old = t;
    old.modsDir = tmp / "oldmods";
    WriteFile(old.modsDir / "Old Mod" / "a.bin", "OLD");
    WriteFile(old.texturesDir / "RENE8P" / "Old Pack" / "tex1_8x8_00000000000000dd_3.png", "P");
    WriteFile(old.modsDir / "gamebanana.ini", "; old\n\n[80]\nname = Old\nfile = 800\nversion = 2\nmod = Old Mod\n"
                                              "mod = ../outside\ntexture = RENE8P/Old Pack\ntexture = RENE8P/../..\n");
    recs = ModInstall_Records(old.modsDir);
    CHECK(recs.size() == 1 && recs[0].modId == 80 && recs[0].fileId == 800 && recs[0].name == "Old" && recs[0].version == "2");
    CHECK(recs[0].mods.size() == 1 && recs[0].mods[0] == "Old Mod" && recs[0].textures.size() == 1 && recs[0].textures[0] == "RENE8P/Old Pack");
    CHECK(fs::exists(old.modsDir / "gamebanana.json") && !fs::exists(old.modsDir / "gamebanana.ini") &&
          fs::exists(old.modsDir / "gamebanana.ini.bak"));
    CHECK(ModInstall_Records(old.modsDir).size() == 1);   // read from the JSON file now
    CHECK(ModInstall_Remove(old, 80, msg) && !fs::exists(old.modsDir / "Old Mod") &&
          !fs::exists(old.texturesDir / "RENE8P" / "Old Pack"));
    CHECK(ReadText(tmp / "outside" / "keep.txt") == "KEEP" && ModInstall_Records(old.modsDir).empty());

    // Windows device names get a '_' (archive entries and names made from mod names)
    WriteArchive(arc / "devices.zip", false, { { "nul/a.bin", "A" }, { "nul/con.txt", "C" }, { "nul/Aux.Tar.gz", "X" },
                                               { "nul/lpt\xC2\xB9.txt", "L" }, { "nul/com1 .txt", "M" }, { "nul/console.txt", "K" } });
    r = ModInstall_Archive(arc / "devices.zip", src(40, "Devices"), t);
    CHECK(r.ok && r.modFolders.size() == 1 && r.modFolders[0] == "nul_");
    fs::path dev = t.modsDir / "nul_";
    CHECK(fs::exists(dev / "a.bin") && fs::exists(dev / "con_.txt") && fs::exists(dev / "Aux_.Tar.gz") &&
          fs::exists(dev / fs::path(u8"lpt\u00B9_.txt")) && fs::exists(dev / "console.txt"));
    WriteArchive(arc / "com.zip", false, { { "a.bin", "A" } });
    r = ModInstall_Archive(arc / "com.zip", src(41, "COM1"), t);
    CHECK(r.ok && r.modFolders.size() == 1 && r.modFolders[0] == "COM1_" && fs::exists(t.modsDir / "COM1_" / "a.bin"));

    // only zip, 7z and RAR open: not tar, nor mtree (whose entries name files on the PC)
    {
        archive* a = archive_write_new();
        archive_write_set_format_pax_restricted(a);
        archive_write_open_filename_w(a, (arc / "mod.tar").c_str());
        archive_entry* en = archive_entry_new();
        archive_entry_set_pathname(en, "Tar/a.bin");
        archive_entry_set_filetype(en, AE_IFREG);
        archive_entry_set_size(en, 1);
        archive_write_header(a, en);
        archive_write_data(a, "T", 1);
        archive_entry_free(en);
        archive_write_close(a);
        archive_write_free(a);
    }
    r = ModInstall_Archive(arc / "mod.tar", src(50, "Tar"), t);
    CHECK(!r.ok && r.message.find("cannot open the archive") != std::string::npos);
    WriteFile(tmp / "secret.bin", "SECRET");
    std::string secret = (tmp / "secret.bin").string();
    std::replace(secret.begin(), secret.end(), '\\', '/');
    WriteFile(arc / "steal.mtree", "#mtree\nSteal/a.bin type=file contents=" + secret + "\n");
    r = ModInstall_Archive(arc / "steal.mtree", src(51, "Steal"), t);
    CHECK(!r.ok && !fs::exists(t.modsDir / "Steal"));

    // RAR4 (stored) and RAR5 (compressed): libarchive's own test archives (it cannot write RAR)
    fs::path data = BKR_MODS_TEST_DATA;
    r = ModInstall_Archive(data / "rar4.rar", src(60, "Rar Four"), t);
    printf("  -> %s\n", r.message.c_str());
    CHECK(r.ok && r.modFolders.size() == 1 && ReadText(t.modsDir / "Rar Four" / "test.txt") == "test text document\r\n" &&
          ReadText(t.modsDir / "Rar Four" / "testdir" / "test.txt") == "test text document\r\n" &&
          !fs::exists(t.modsDir / "Rar Four" / "testlink"));   // a symbolic link: skipped
    r = ModInstall_Archive(data / "rar5.rar", src(61, "Rar Five"), t);
    printf("  -> %s\n", r.message.c_str());
    CHECK(r.ok && fs::file_size(t.modsDir / "Rar Five" / "test.bin", ec) == 1200);

    // too many entries
    {
        archive* a = archive_write_new();
        archive_write_set_format_zip(a);
        archive_write_open_filename_w(a, (arc / "many.zip").c_str());
        for (int i = 0; i <= 100000; i++)
        {
            archive_entry* en = archive_entry_new();
            archive_entry_set_pathname(en, std::format("../skip{}", i).c_str());   // not unpacked: no files written
            archive_entry_set_filetype(en, AE_IFREG);
            archive_entry_set_size(en, 0);
            archive_write_header(a, en);
            archive_entry_free(en);
        }
        archive_write_close(a);
        archive_write_free(a);
    }
    r = ModInstall_Archive(arc / "many.zip", src(70, "Many"), t);
    printf("  -> %s\n", r.message.c_str());
    CHECK(!r.ok && r.message.find("more than 100000 files") != std::string::npos);

    // downloads: https only (checked before any network use)
    std::string error;
    CHECK(!Http_Download("http://gamebanana.com/dl/1", tmp / "x.zip", error) && error.find("https") != std::string::npos);
    CHECK(!Http_Download("file:///C:/Windows/win.ini", tmp / "x.zip", error) && !fs::exists(tmp / "x.zip"));

    CHECK(!fs::exists(t.modsDir / ".staging"));
    fs::remove_all(tmp, ec);
}

static void ParseTests()
{
    const char* list = R"({"_aMetadata":{"_nRecordCount":3,"_bIsComplete":true},"_aRecords":[
        {"_idRow":592320,"_sModelName":"Mod","_sName":"Silver","_bHasFiles":true,"_tsDateModified":200,"_tsDateAdded":100,
         "_nViewCount":7,"_aSubmitter":{"_sName":"Kyo"},"_aRootCategory":{"_sName":"Skins"},
         "_aPreviewMedia":{"_aImages":[{"_sBaseUrl":"https://images.gamebanana.com/img/ss/mods","_sFile":"a.jpg","_sFile220":"220-90_a.jpg"}]}},
        {"_idRow":20945,"_sModelName":"Tool","_sName":"Manager","_bHasFiles":true},
        {"_idRow":96515,"_sModelName":"Request","_sName":"Please","_bHasFiles":false}]})";
    std::vector<GbMod> mods;
    bool complete = false;
    CHECK(Gb_ParseList(list, mods, complete) && complete && mods.size() == 1);
    CHECK(mods[0].id == 592320 && mods[0].author == "Kyo" && mods[0].category == "Skins" && mods[0].views == 7);
    CHECK(mods[0].thumb == "https://images.gamebanana.com/img/ss/mods/220-90_a.jpg");
    const char* details = R"({"_idRow":592320,"_sName":"Silver","_sDescription":"A knight","_nLikeCount":21,"_nDownloadCount":275,
        "_sText":"<b>Hi</b> &amp; welcome<br>line 2<ul><li>one</li><li>two</li></ul>&#9733;",
        "_aFiles":[{"_idRow":1,"_sFile":"old.rar","_bIsArchived":true,"_tsDateAdded":50,"_sDownloadUrl":"https://x/1","_sAnalysisResult":"ok"},
                   {"_idRow":2,"_sFile":"new.rar","_bIsArchived":false,"_tsDateAdded":60,"_nFilesize":1234,"_sDownloadUrl":"https://x/2",
                    "_sMd5Checksum":"abc","_sAnalysisResult":"ok","_sAvResult":"clean","_sVersion":"2"},
                   {"_idRow":3,"_sFile":"bad.rar","_tsDateAdded":40,"_sDownloadUrl":"https://x/3","_sAnalysisResult":"contains_exe"}]})";
    GbMod m = mods[0];
    CHECK(Gb_ParseDetails(details, m) && m.details && m.likes == 21 && m.downloads == 275 && m.summary == "A knight");
    CHECK(m.files.size() == 3 && m.files[0].id == 2 && m.files[0].version == "2" && m.files[0].size == 1234 && m.files[0].clean);
    CHECK(m.files[1].id == 3 && !m.files[1].clean && m.files[2].archived);
    printf("  text: [%s]\n", m.text.c_str());
    CHECK(m.text == "Hi & welcome\nline 2\n- one\n- two\n\xE2\x98\x85");
    CHECK(!Gb_ParseList("<html>busy</html>", mods, complete));
}

// --install: a real archive or GameBanana download, with the game's code
static int InstallCommand(int argc, char** argv)
{
    std::string what, data, mods, textures;
    for (int i = 1; i + 1 < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--install") what = argv[++i];
        else if (a == "--data") data = argv[++i];
        else if (a == "--mods") mods = argv[++i];
        else if (a == "--textures") textures = argv[++i];
    }
    InstallTargets t;
    t.modsDir = mods;
    t.texturesDir = textures;
    t.gameId = "RENE8P";
    t.discFiles = ModInstall_DiscFiles(data);
    t.recycle = false;   // a test folder: replaced files are deleted
    printf("disc: %zu files\n", t.discFiles.size());
    if (t.discFiles.empty()) return 2;
    InstallSource src;
    fs::path archive = what;
    std::error_code ec;
    if (what.rfind("gamebanana:", 0) == 0)
    {
        GbMod m;
        m.id = strtoull(what.c_str() + 11, nullptr, 10);
        std::string error;
        if (!Gb_FetchDetails(m, error) || m.files.empty()) { printf("details: %s\n", error.c_str()); return 2; }
        const GbFile& f = m.files[0];
        printf("%s by %s: %s (%llu bytes, md5 %s)\n", m.name.c_str(), m.author.c_str(), f.name.c_str(),
               (unsigned long long)f.size, f.md5.c_str());
        archive = fs::path(mods) / f.name;
        fs::create_directories(mods, ec);
        HttpProgress p;
        if (!Http_Download(f.url, archive, error, &p)) { printf("download: %s\n", error.c_str()); return 2; }
        printf("downloaded %llu bytes\n", (unsigned long long)p.done.load());
        src.modId = m.id;
        src.fileId = f.id;
        src.name = m.name;
        src.author = m.author;
        src.version = f.version;
        src.fileName = f.name;
        src.fileAdded = f.added;
    }
    else
        src.name = archive.stem().string();
    InstallResult r = ModInstall_Archive(archive, src, t);
    if (what.rfind("gamebanana:", 0) == 0) fs::remove(archive, ec);
    printf("%s: %s\n", r.ok ? "OK" : "NOT INSTALLED", r.message.c_str());
    for (auto& n : r.notes) printf("  note: %s\n", n.c_str());
    for (auto& f : r.modFolders) printf("  mods/%s\n", f.c_str());
    for (auto& f : r.textureFolders) printf("  textures/%s\n", f.c_str());
    return r.ok ? 0 : 1;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--install")) return InstallCommand(argc, argv);
    printf("== disc overlay\n");
    OverlayTests();
    printf("== install\n");
    InstallTests();
    printf("== hostile archives and records\n");
    HardeningTests();
    printf("== GameBanana answers\n");
    ParseTests();
    printf("%s (%d failed)\n", s_failed ? "FAILED" : "ALL PASSED", s_failed);
    return s_failed ? 1 : 0;
}
