#include "mod_install.h"
#include "mods.h"
#include "../log.h"

#include <archive.h>
#include <archive_entry.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <tuple>
#include <windows.h>
#include <shellapi.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
// archive bombs: a mod for this game is well below these
constexpr uint64_t kMaxUnpacked = 4ull << 30;
constexpr size_t kMaxEntries = 100000;

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

// without the long-path prefix (messages, the shell)
fs::path Plain(const fs::path& p)
{
    const std::wstring& w = p.native();
    if (w.rfind(LR"(\\?\UNC\)", 0) == 0) return fs::path(L"\\\\" + w.substr(8));
    if (w.rfind(LR"(\\?\)", 0) == 0) return fs::path(w.substr(4));
    return p;
}

std::string Utf8(const fs::path& p)
{
    auto u = Plain(p).u8string();
    return std::string(u.begin(), u.end());
}

// a '/'-separated relative path (long-path folders need backslashes)
fs::path FromUtf8(const std::string& s)
{
    fs::path p(std::u8string(s.begin(), s.end()));
    p.make_preferred();
    return p;
}

std::string Trim(std::string s)
{
    auto ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && ws(s[i])) i++;
    return s.substr(i);
}

std::vector<std::string> Split(const std::string& path)
{
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path + "/")
        if (c == '/' || c == '\\')
        {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        }
        else
            cur += c;
    return parts;
}

std::string Join(const std::vector<std::string>& parts, size_t from, size_t to)
{
    std::string s;
    for (size_t i = from; i < to; i++) s += (s.empty() ? "" : "/") + parts[i];
    return s;
}

// Windows device names (CON, NUL, COM1, ...), alone or with an extension: under \\?\ paths
// they become real files that Explorer cannot delete
bool IsDeviceName(const std::string& name)
{
    std::string stem = Lower(name.substr(0, name.find('.')));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    static const std::set<std::string> devices = [] {
        std::set<std::string> d = { "con", "prn", "aux", "nul", "conin$", "conout$" };
        for (const char* digit : { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "\xC2\xB9", "\xC2\xB2", "\xC2\xB3" })
        {
            d.insert(std::string("com") + digit);
            d.insert(std::string("lpt") + digit);
        }
        return d;
    }();
    return devices.count(stem) != 0;
}

// a name that is safe as one folder name
std::string SafeName(const std::string& name)
{
    std::string s;
    for (unsigned char c : name)
        s += c < 32 || c == 127 || strchr("<>:\"/\\|?*", c) ? '_' : (char)c;
    s = Trim(s);
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    if (s.empty() || s[0] == '.') s = "mod" + s;
    s = s.substr(0, 100);
    if (IsDeviceName(s)) s.insert(std::min(s.find('.'), s.size()), "_");
    return s;
}

// text for one line of mod.ini (text from GameBanana could otherwise add lines)
std::string OneLine(const std::string& text)
{
    std::string s;
    for (unsigned char c : text) s += c < 32 || c == 127 ? ' ' : (char)c;
    return Trim(s);
}

// a folder of a record (a text file anyone can edit): depth safe folder names, so it is
// right below mods/ (1) or textures/ (2: <game ID>/<name>)
bool ValidRecordFolder(const std::string& rel, size_t depth)
{
    if (rel.find('\\') != std::string::npos) return false;
    std::vector<std::string> parts = Split(rel);
    if (parts.size() != depth || Join(parts, 0, parts.size()) != rel) return false;
    for (auto& p : parts)
        if (p == "." || p == ".." || p.find(':') != std::string::npos || SafeName(p) != p) return false;
    return true;
}

// an archive entry's path as a safe relative path, or empty to skip it
std::string SafeEntryPath(const archive_entry* e)
{
    std::string path;
    if (const wchar_t* w = archive_entry_pathname_w(const_cast<archive_entry*>(e)))
        path = Utf8(fs::path(w));
    else if (const char* u = archive_entry_pathname_utf8(const_cast<archive_entry*>(e)))
        path = u;
    else if (const char* n = archive_entry_pathname(const_cast<archive_entry*>(e)))
        path = n;
    std::vector<std::string> parts;
    for (auto& p : Split(path))
    {
        if (p == ".") continue;
        if (p == ".." || p.find(':') != std::string::npos) return {};   // outside the folder, drive or stream
        parts.push_back(SafeName(p) == p ? p : SafeName(p));
    }
    return Join(parts, 0, parts.size());
}

bool Extract(const fs::path& file, const fs::path& dest, std::string& error, std::atomic<bool>* cancel)
{
    // only the formats mods come in, no compression filters (these formats compress
    // inside). Not "all": mtree lists files by path, which would read files on the PC.
    archive* a = archive_read_new();
    archive_read_support_format_zip(a);
    archive_read_support_format_7zip(a);
    archive_read_support_format_rar(a);
    archive_read_support_format_rar5(a);
    if (archive_read_open_filename_w(a, file.c_str(), 1 << 16) != ARCHIVE_OK)
    {
        error = std::format("cannot open the archive: {}", archive_error_string(a) ? archive_error_string(a) : "unknown format");
        archive_read_free(a);
        return false;
    }
    bool ok = true;
    uint64_t total = 0;
    size_t entries = 0;
    std::vector<char> buf(1 << 16);
    archive_entry* e = nullptr;
    int r;
    while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN)
    {
        if (cancel && *cancel) { error = "canceled"; ok = false; break; }
        if (++entries > kMaxEntries)
        {
            error = std::format("the archive has more than {} files", kMaxEntries);
            ok = false;
            break;
        }
        if (archive_entry_is_encrypted(e))
        {
            error = "the archive is password-protected";
            ok = false;
            break;
        }
        std::string rel = SafeEntryPath(e);
        if (rel.empty()) continue;
        fs::path out = dest / FromUtf8(rel);
        std::error_code ec;
        if (archive_entry_filetype(e) == AE_IFDIR)
        {
            fs::create_directories(out, ec);
            continue;
        }
        if (archive_entry_filetype(e) != AE_IFREG) continue;   // links, devices
        fs::create_directories(out.parent_path(), ec);
        FILE* f = _wfopen(out.c_str(), L"wb");
        if (!f)
        {
            error = "cannot write " + Utf8(out);
            ok = false;
            break;
        }
        la_ssize_t n;
        while ((n = archive_read_data(a, buf.data(), buf.size())) > 0)
        {
            total += (uint64_t)n;
            if (total > kMaxUnpacked || fwrite(buf.data(), 1, (size_t)n, f) != (size_t)n) { n = -1; break; }
        }
        fclose(f);
        if (n < 0)
        {
            error = total > kMaxUnpacked ? std::format("the archive unpacks to more than {} GB", kMaxUnpacked >> 30)
                                         : std::format("cannot unpack {}: {}", rel, archive_error_string(a) ? archive_error_string(a) : "error");
            ok = false;
            break;
        }
    }
    if (ok && r != ARCHIVE_EOF)
    {
        error = std::format("cannot read the archive: {}", archive_error_string(a) ? archive_error_string(a) : "error");
        ok = false;
    }
    archive_read_free(a);
    return ok;
}

// every file under dir, relative and '/'-separated (UTF-8)
std::vector<std::string> ListFiles(const fs::path& dir)
{
    std::vector<std::string> out;
    std::error_code ec;
    fs::path root = Mods_LongPath(dir);
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string rel = Utf8(it->path().lexically_relative(root));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        out.push_back(rel);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string Ext(const std::string& name)
{
    size_t dot = name.rfind('.');
    size_t slash = name.rfind('/');
    return dot == std::string::npos || (slash != std::string::npos && dot < slash) ? "" : Lower(name.substr(dot));
}

std::string FileName(const std::string& rel)
{
    size_t slash = rel.rfind('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

bool IsGenericFolder(const std::string& name)
{
    static const std::set<std::string> generic = { "files", "data", "mod", "mods", "extracted", "root", "disc",
                                                   "dvd", "game", "partition", "content", "install", "rene8p",
                                                   "renp8p", "renj8p" };
    return generic.count(Lower(name)) != 0;
}

bool IsGameIdFolder(const std::string& name)
{
    std::string l = Lower(name);
    for (const char* id : { "rene8p", "renp8p", "renj8p", "rene", "renp", "renj", "ren" })
        if (l == id) return true;
    return false;
}

// moves (or copies, across volumes) src to dst; dst must not exist
bool MoveTree(const fs::path& src, const fs::path& dst, std::string& error)
{
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::rename(src, dst, ec);
    if (!ec) return true;
    ec.clear();
    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        error = std::format("cannot copy to {}: {}", Utf8(dst), ec.message());
        return false;
    }
    fs::remove_all(src, ec);
    return true;
}

// p is below root (not root itself)
bool Below(const fs::path& p, const fs::path& root)
{
    std::error_code ec;
    fs::path rel = fs::absolute(Plain(p), ec).lexically_normal().lexically_relative(fs::absolute(Plain(root), ec).lexically_normal());
    return !rel.empty() && rel != "." && *rel.begin() != "..";
}

// to the Recycle Bin (deleted outright where there is none); only below root
bool Recycle(const fs::path& p, const fs::path& root, bool recycle = true)
{
    std::error_code ec;
    if (!Below(p, root))
    {
        LOG_WARN("mods: not removing {}: not in {}", Utf8(p), Utf8(root));
        return false;
    }
    if (!fs::exists(p, ec)) return true;
    if (!recycle)
    {
        fs::remove_all(p, ec);
        return !fs::exists(p, ec);
    }
    std::wstring from = fs::absolute(Plain(p), ec).wstring();
    from.push_back(L'\0');   // double-null terminated list
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted && !fs::exists(p, ec)) return true;
    fs::remove_all(p, ec);
    return !fs::exists(p, ec);
}

std::string Today()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    return std::format("{:04}-{:02}-{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

// What was installed from GameBanana (for updates and removal). JSON, so text from
// GameBanana is always escaped; still read as untrusted input (anyone can edit it).
fs::path RecordsPath(const fs::path& modsDir) { return modsDir / "gamebanana.json"; }
fs::path LegacyRecordsPath(const fs::path& modsDir) { return modsDir / "gamebanana.ini"; }   // before JSON

bool WriteRecords(const fs::path& modsDir, const std::vector<InstalledMod>& recs)
{
    json list = json::array();
    for (auto& r : recs)
    {
        json mods = json::array(), textures = json::array();
        for (auto& m : r.mods)
            if (ValidRecordFolder(m, 1)) mods.push_back(m);
        for (auto& t : r.textures)
            if (ValidRecordFolder(t, 2)) textures.push_back(t);
        list.push_back({ { "id", r.modId }, { "file", r.fileId }, { "name", r.name }, { "file_name", r.fileName },
                         { "version", r.version }, { "file_added", r.fileAdded }, { "date", r.date },
                         { "mods", mods }, { "textures", textures } });
    }
    json doc = { { "comment", "Mods installed from GameBanana by the game's mod browser (for updates and removal)" },
                 { "records", list } };
    // invalid UTF-8 in a name becomes U+FFFD instead of failing
    std::string s = doc.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
    std::error_code ec;
    fs::create_directories(modsDir, ec);
    fs::path tmp = RecordsPath(modsDir);
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << s;
        if (!f) return false;
    }
    fs::rename(tmp, RecordsPath(modsDir), ec);   // replaces the old file
    return !ec;
}

std::vector<InstalledMod> ReadJsonRecords(const fs::path& file)
{
    std::vector<InstalledMod> recs;
    std::error_code ec;
    if (!fs::exists(file, ec)) return recs;   // nothing installed from GameBanana yet
    std::ifstream f(file, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("records") || !doc["records"].is_array())
    {
        LOG_WARN("mods: {} is not a valid records file; ignoring it", Utf8(file));
        return recs;
    }
    auto str = [](const json& o, const char* key) { return o.contains(key) && o[key].is_string() ? o[key].get<std::string>() : ""; };
    auto num = [](const json& o, const char* key) -> int64_t {
        return o.contains(key) && o[key].is_number_integer() ? o[key].get<int64_t>() : 0;
    };
    for (auto& o : doc["records"])
    {
        if (!o.is_object() || num(o, "id") <= 0)
        {
            LOG_WARN("mods: {}: ignoring a record without a mod id", Utf8(file));
            continue;
        }
        InstalledMod r;
        r.modId = (uint64_t)num(o, "id");
        r.fileId = (uint64_t)std::max<int64_t>(0, num(o, "file"));
        r.name = str(o, "name");
        r.fileName = str(o, "file_name");
        r.version = str(o, "version");
        r.fileAdded = num(o, "file_added");
        r.date = str(o, "date");
        for (auto [key, depth, out] : { std::tuple{ "mods", size_t(1), &r.mods }, std::tuple{ "textures", size_t(2), &r.textures } })
        {
            if (!o.contains(key)) continue;
            if (!o[key].is_array()) { LOG_WARN("mods: {}: {} of {} is not a list; ignoring it", Utf8(file), key, r.modId); continue; }
            // only folders right below mods/ and textures/<game ID>/
            for (auto& v : o[key])
                if (v.is_string() && ValidRecordFolder(v.get<std::string>(), depth)) out->push_back(v.get<std::string>());
                else LOG_WARN("mods: {}: ignoring {} entry {} of {}", Utf8(file), key, v.dump(-1, ' ', false, json::error_handler_t::replace), r.modId);
        }
        recs.push_back(r);
    }
    return recs;
}

// gamebanana.ini (the mod browser's first versions): one [<mod id>] section per mod
std::vector<InstalledMod> ReadLegacyRecords(const fs::path& file)
{
    std::vector<InstalledMod> recs;
    std::ifstream f(file);
    std::string line;
    while (std::getline(f, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[')
        {
            InstalledMod r;
            r.modId = strtoull(line.c_str() + 1, nullptr, 10);
            if (r.modId) recs.push_back(r);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || recs.empty()) continue;
        std::string key = Lower(Trim(line.substr(0, eq))), val = Trim(line.substr(eq + 1));
        InstalledMod& r = recs.back();
        if (key == "name") r.name = val;
        else if (key == "file") r.fileId = strtoull(val.c_str(), nullptr, 10);
        else if (key == "file_name") r.fileName = val;
        else if (key == "version") r.version = val;
        else if (key == "file_added") r.fileAdded = atoll(val.c_str());
        else if (key == "date") r.date = val;
        else if (key == "mod" || key == "texture")
        {
            // only folders right below mods/ and textures/<game ID>/
            if (ValidRecordFolder(val, key == "mod" ? 1 : 2)) (key == "mod" ? r.mods : r.textures).push_back(val);
            else LOG_WARN("mods: gamebanana.ini: ignoring {} = {}", key, val);
        }
    }
    return recs;
}
}   // namespace

std::vector<InstalledMod> ModInstall_Records(const fs::path& modsDir)
{
    std::error_code ec;
    fs::path file = RecordsPath(modsDir), legacy = LegacyRecordsPath(modsDir);
    if (!fs::exists(file, ec) && fs::exists(legacy, ec))
    {
        // once: gamebanana.ini -> gamebanana.json (the old file stays as gamebanana.ini.bak)
        std::vector<InstalledMod> recs = ReadLegacyRecords(legacy);
        if (WriteRecords(modsDir, recs))
        {
            fs::path bak = legacy;
            bak += ".bak";
            fs::rename(legacy, bak, ec);
            LOG_INFO("mods: moved {} records from gamebanana.ini to gamebanana.json", recs.size());
        }
        return recs;
    }
    return ReadJsonRecords(file);
}

fs::path ModInstall_TexturesDir(const std::string& configuredDir)
{
    if (!configuredDir.empty()) return FromUtf8(configuredDir);
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    fs::path exe = fs::path(buf).parent_path();
    std::error_code ec;
    for (fs::path p : { exe / "textures", exe / "game" / "textures", exe / ".." / "game" / "textures" })
        if (fs::is_directory(p, ec)) return fs::weakly_canonical(p, ec);
    return exe / "textures";
}

std::vector<std::string> ModInstall_DiscFiles(const fs::path& dataDir)
{
    std::vector<std::string> files;
    std::ifstream f(dataDir / "sys" / "fst.bin", std::ios::binary);
    std::vector<uint8_t> fst((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto be32 = [&](size_t at) { return (uint32_t)fst[at] << 24 | fst[at + 1] << 16 | fst[at + 2] << 8 | fst[at + 3]; };
    if (fst.size() < 12) return files;
    uint32_t n = be32(8);
    if ((uint64_t)n * 12 > fst.size()) return files;
    const char* strings = (const char*)fst.data() + n * 12;
    size_t stringsSize = fst.size() - n * 12;
    std::vector<std::pair<uint32_t, std::string>> dirs = { { n, "" } };
    for (uint32_t i = 1; i < n; i++)
    {
        while (dirs.size() > 1 && i >= dirs.back().first) dirs.pop_back();
        uint32_t nameOff = be32(i * 12) & 0xFFFFFF;
        if (nameOff >= stringsSize) break;
        std::string name(strings + nameOff, strnlen(strings + nameOff, stringsSize - nameOff));
        std::string rel = dirs.back().second.empty() ? name : dirs.back().second + "/" + name;
        if (fst[i * 12]) dirs.push_back({ be32(i * 12 + 8), rel });
        else files.push_back(rel);
    }
    return files;
}

InstallResult ModInstall_Archive(const fs::path& archive, const InstallSource& src, const InstallTargets& targets,
                                 std::atomic<bool>* cancel)
{
    // archives nest deep folders: long paths throughout
    InstallTargets t = targets;
    t.modsDir = Mods_LongPath(t.modsDir);
    t.texturesDir = Mods_LongPath(t.texturesDir);
    InstallResult res;
    std::error_code ec;
    std::string label = src.name.empty() ? Utf8(archive.stem()) : src.name;
    fs::path staging = t.modsDir / ".staging" / std::format("{}_{}", src.modId, GetTickCount64());
    fs::create_directories(staging, ec);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); fs::remove(p.parent_path(), e); } } cleanup{ staging };
    std::string error;
    if (!Extract(archive, staging, error, cancel))
    {
        res.message = std::format("{}: {}", label, error);
        LOG_WARN("mods: install {} failed: {}", label, error);
        return res;
    }
    std::vector<std::string> files = ListFiles(staging);
    LOG_INFO("mods: {} unpacked: {} files", label, files.size());

    std::set<std::string> disc;
    for (auto& d : t.discFiles) disc.insert(Lower(d));

    // what the archive holds
    std::map<std::string, int> roots;            // game-file mirror root -> files
    std::map<std::string, std::string> texRoots; // texture pack folder -> game ID folder name ("" if none)
    std::vector<std::string> code, cheats, dolphin, riivolution;
    for (auto& rel : files)
    {
        std::vector<std::string> parts = Split(rel);
        std::string name = Lower(parts.back()), ext = Ext(rel);
        if (ext == ".dol" || ext == ".elf" || ext == ".xdelta" || ext == ".bps" || ext == ".ips" || ext == ".ppf" ||
            ext == ".vcdiff")
        {
            code.push_back(rel);
            continue;
        }
        if (ext == ".gct") { cheats.push_back(rel); continue; }
        if (ext == ".xml" && Lower(rel).find("riivolution") != std::string::npos) { riivolution.push_back(rel); continue; }
        if (ext == ".ini" && name != "mod.ini")
        {
            // Dolphin GameSettings (<game ID>.ini: codes, patches, graphics) and profiles
            std::ifstream f(staging / FromUtf8(rel));
            std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::string l = Lower(text);
            if (l.find("[gecko") != std::string::npos || l.find("[actionreplay") != std::string::npos ||
                l.find("[onframe") != std::string::npos)
                cheats.push_back(rel);
            else if (IsGameIdFolder(name.substr(0, name.size() - 4)) || l.find("[profile]") != std::string::npos ||
                     l.find("[video_") != std::string::npos || l.find("[core]") != std::string::npos ||
                     l.find("[wiimote") != std::string::npos || l.find("[gcpad") != std::string::npos)
                dolphin.push_back(rel);
            // other .ini files are game files or the mod's own notes
        }
        if ((ext == ".png" || ext == ".dds") && name.rfind("tex1_", 0) == 0)
        {
            // the pack's folder: its (innermost) game ID folder (Dolphin's layout), else its top folder
            size_t k = parts.size() - 1;
            for (size_t i = 0; i + 1 < parts.size(); i++)
                if (IsGameIdFolder(parts[i])) k = i;
            if (k < parts.size() - 1) texRoots.emplace(Join(parts, 0, k + 1), parts[k]);
            else texRoots.emplace(parts.size() > 1 ? parts[0] : "", "");
            continue;
        }
        // the longest tail of the path that is a disc file: the rest is the mirror's root
        for (size_t k = 0; k < parts.size(); k++)
            if (disc.count(Lower(Join(parts, k, parts.size()))))
            {
                roots[Join(parts, 0, k)]++;
                break;
            }
    }
    if (!code.empty())
    {
        res.message = std::format("{} changes the game's code ({}). The PC version runs recompiled code and cannot "
                                  "load code patches, so it was not installed.",
                                  label, FileName(code[0]));
        LOG_WARN("mods: {} not installed: code patch {}", label, code[0]);
        return res;
    }
    if (!riivolution.empty())
    {
        res.message = std::format("{} is a Riivolution patch, which the PC version does not support; it was not installed.", label);
        return res;
    }
    if (!cheats.empty()) res.notes.push_back(std::format("Skipped cheat/patch codes ({}): the PC version cannot use them.", FileName(cheats[0])));
    if (!dolphin.empty()) res.notes.push_back(std::format("Skipped Dolphin settings ({}): use the game's own settings instead.", FileName(dolphin[0])));

    // mirror roots nested in another one (an "Additional Files" folder holding an
    // alternative file) belong to that mod
    std::vector<std::pair<std::string, int>> mirror;
    for (auto& [root, count] : roots)
    {
        bool nested = false;
        for (auto& [other, n] : roots)
            if (other != root && (other.empty() || root.rfind(other + "/", 0) == 0) && n >= count) nested = true;
        if (!nested) mirror.push_back({ root, count });
    }
    std::sort(mirror.begin(), mirror.end(), [](auto& a, auto& b) { return a.second > b.second; });

    // earlier install of this mod: replace it (same folder names keep their place in the load order)
    std::vector<InstalledMod> recs = ModInstall_Records(t.modsDir);
    InstalledMod* prev = nullptr;
    for (auto& r : recs)
        if (src.modId && r.modId == src.modId) prev = &r;

    std::set<std::string> used;
    for (size_t i = 0; i < mirror.size(); i++)
    {
        std::vector<std::string> parts = Split(mirror[i].first);
        // keep the folder that holds files/ (with its mod.ini)
        if (!parts.empty() && Lower(parts.back()) == "files") parts.pop_back();
        fs::path from = staging / FromUtf8(Join(parts, 0, parts.size()));
        std::string name;
        // an update keeps the folder (checked again: the records are a file anyone can edit)
        if (prev && i < prev->mods.size() && ValidRecordFolder(prev->mods[i], 1)) name = prev->mods[i];
        else if (!parts.empty() && !IsGenericFolder(parts.back())) name = SafeName(parts.back());
        else name = SafeName(label);
        // a folder that is not this mod's earlier install (another mod, one copied by hand)
        // stays: take another name
        auto owned = [&](const std::string& n) {
            return prev && std::any_of(prev->mods.begin(), prev->mods.end(), [&](const std::string& m) { return Lower(m) == Lower(n); });
        };
        std::string base = name;
        for (int n = 2; used.count(Lower(name)) || (fs::exists(t.modsDir / FromUtf8(name), ec) && !owned(name)); n++)
            name = std::format("{} ({})", base, n);
        used.insert(Lower(name));
        fs::path to = t.modsDir / FromUtf8(name);
        if (fs::exists(to, ec))
        {
            LOG_INFO("mods: replacing {} (old files to the Recycle Bin)", Utf8(to));
            if (!Recycle(to, t.modsDir, t.recycle))
            {
                res.message = std::format("{}: cannot replace {}", label, Utf8(to));
                return res;
            }
        }
        if (from == staging)
        {
            // the archive itself is the mirror: move its top level (mod.ini included)
            fs::create_directories(to, ec);
            for (auto& e : fs::directory_iterator(staging, ec))
                if (!MoveTree(e.path(), to / e.path().filename(), error)) { res.message = error; return res; }
        }
        else if (!MoveTree(from, to, error))
        {
            res.message = error;
            return res;
        }
        if (!fs::exists(to / "mod.ini", ec) && !fs::exists(to / "files" / "mod.ini", ec))
        {
            std::ofstream ini(to / "mod.ini", std::ios::binary);
            ini << "[Mod]\nname = " << OneLine(mirror.size() > 1 ? name : label) << "\n";
            if (!src.author.empty()) ini << "author = " << OneLine(src.author) << "\n";
            if (!src.version.empty()) ini << "version = " << OneLine(src.version) << "\n";
        }
        res.modFolders.push_back(name);
        LOG_INFO("mods: installed {} ({} game files) to {}", label, mirror[i].second, Utf8(to));
    }

    // texture packs: textures/<game ID>/<mod name>/
    std::map<std::string, std::vector<std::string>> packsById;   // ID folder -> pack folders
    for (auto& [root, id] : texRoots) packsById[id.empty() ? t.gameId : id].push_back(root);
    std::string texBase = prev && !prev->textures.empty() && ValidRecordFolder(prev->textures[0], 2)
                              ? Split(prev->textures[0]).back()
                              : SafeName(label);
    for (auto& [id, packRoots] : packsById)
    {
        std::string texName = texBase;
        auto owned = [&](const std::string& rel) {
            return prev && std::any_of(prev->textures.begin(), prev->textures.end(), [&](const std::string& x) { return Lower(x) == Lower(rel); });
        };
        for (int n = 2; fs::exists(t.texturesDir / FromUtf8(id) / FromUtf8(texName), ec) && !owned(id + "/" + texName); n++)
            texName = std::format("{} ({})", texBase, n);
        std::string rel = id + "/" + texName;
        fs::path to = t.texturesDir / FromUtf8(id) / FromUtf8(texName);
        if (fs::exists(to, ec) && !Recycle(to, t.texturesDir, t.recycle))
        {
            res.message = std::format("{}: cannot replace {}", label, Utf8(to));
            return res;
        }
        fs::create_directories(to, ec);
        size_t count = 0;
        for (auto& root : packRoots)
        {
            fs::path from = staging / FromUtf8(root);
            for (auto& rel2 : ListFiles(from))
            {
                std::string ext = Ext(rel2), name = Lower(FileName(rel2));
                if ((ext != ".png" && ext != ".dds") || name.rfind("tex1_", 0) != 0) continue;
                fs::path dst = to / FromUtf8(rel2);
                fs::create_directories(dst.parent_path(), ec);
                fs::rename(from / FromUtf8(rel2), dst, ec);
                if (ec) { ec.clear(); fs::copy_file(from / FromUtf8(rel2), dst, fs::copy_options::overwrite_existing, ec); }
                count++;
            }
        }
        res.textureFolders.push_back(rel);
        LOG_INFO("mods: installed {} textures of {} to {}", count, label, Utf8(to));
    }

    if (res.modFolders.empty() && res.textureFolders.empty())
    {
        if (!res.notes.empty())
            res.message = std::format("{} only has Dolphin settings or codes; there is nothing the PC version can use.", label);
        else
            res.message = std::format("{} has no game files or textures of this game that the PC version knows, so it "
                                      "was not installed. It may need manual steps: see its page.",
                                      label);
        LOG_WARN("mods: {} not installed: {}", label, res.message);
        return res;
    }

    // GameBanana installs are recorded for updates and removal
    if (src.modId)
    {
        InstalledMod r;
        r.modId = src.modId;
        r.fileId = src.fileId;
        r.name = src.name;
        r.fileName = src.fileName;
        r.version = src.version;
        r.fileAdded = src.fileAdded;
        r.date = Today();
        r.mods = res.modFolders;
        r.textures = res.textureFolders;
        std::erase_if(recs, [&](const InstalledMod& x) { return x.modId == src.modId; });
        recs.push_back(r);
        WriteRecords(t.modsDir, recs);
    }
    res.ok = true;
    std::string what;
    if (!res.modFolders.empty())
        what = res.modFolders.size() == 1 ? "as a mod" : std::format("as {} mods (variants; enable the one you want)", res.modFolders.size());
    if (!res.textureFolders.empty()) what += std::string(what.empty() ? "as" : " and") + " a texture pack";
    res.message = std::format("Installed {} {}.", label, what);
    return res;
}

bool ModInstall_Remove(const InstallTargets& t, uint64_t modId, std::string& message)
{
    std::vector<InstalledMod> recs = ModInstall_Records(t.modsDir);
    auto it = std::find_if(recs.begin(), recs.end(), [&](const InstalledMod& r) { return r.modId == modId; });
    if (it == recs.end()) { message = "not installed"; return false; }
    // only folders below mods/ and textures/<game ID>/ (the record is a text file anyone can edit)
    bool ok = true;
    for (auto& m : it->mods) ok &= ValidRecordFolder(m, 1) && Recycle(t.modsDir / FromUtf8(m), t.modsDir, t.recycle);
    for (auto& tx : it->textures)
        ok &= ValidRecordFolder(tx, 2) && Recycle(t.texturesDir / FromUtf8(tx), t.texturesDir, t.recycle);
    message = ok ? std::format("Removed {} (moved to the Recycle Bin).", it->name) : std::format("Could not remove all of {}.", it->name);
    LOG_INFO("mods: {}", message);
    recs.erase(it);
    WriteRecords(t.modsDir, recs);
    return ok;
}

bool ModInstall_RemoveFolder(const InstallTargets& t, const std::string& folder, std::string& message)
{
    if (folder.empty() || folder == "." || folder == ".." || folder.find_first_of("/\\:") != std::string::npos)
    {
        message = "not a mod folder: " + folder;
        return false;
    }
    // a GameBanana install: remove all of it
    for (auto& r : ModInstall_Records(t.modsDir))
        for (auto& m : r.mods)
            if (Lower(m) == Lower(folder) && r.mods.size() == 1) return ModInstall_Remove(t, r.modId, message);
    bool ok = Recycle(t.modsDir / FromUtf8(folder), t.modsDir, t.recycle);
    message = ok ? std::format("Removed {} (moved to the Recycle Bin).", folder) : std::format("Could not remove {}.", folder);
    LOG_INFO("mods: {}", message);
    if (ok)
    {
        std::vector<InstalledMod> recs = ModInstall_Records(t.modsDir);
        for (auto& r : recs) std::erase_if(r.mods, [&](const std::string& m) { return Lower(m) == Lower(folder); });
        std::erase_if(recs, [](const InstalledMod& r) { return r.mods.empty() && r.textures.empty(); });
        WriteRecords(t.modsDir, recs);
    }
    return ok;
}
