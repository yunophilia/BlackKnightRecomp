#include "mods.h"
#include "../log.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <windows.h>

namespace fs = std::filesystem;

namespace
{
std::vector<ModInfo> s_loaded;
std::string s_dirOverride;

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

std::string Trim(std::string s)
{
    auto ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && ws(s[i])) i++;
    return s.substr(i);
}

std::string Utf8(const fs::path& p)
{
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// mod.ini: "key = value" lines in any section. Storybook Mod Manager writes [Mod] with
// Name/Author/Version; title and description are accepted as well.
void ReadIni(const fs::path& path, ModInfo& m)
{
    std::ifstream f(path);
    std::string line;
    bool first = true;
    while (std::getline(f, line))
    {
        if (first && line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF)
            line = line.substr(3);
        first = false;
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Lower(Trim(line.substr(0, eq)));
        std::string val = Trim(line.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
        if (val.empty()) continue;
        if (key == "title" || key == "name") m.title = val;
        else if (key == "author") m.author = val;
        else if (key == "version") m.version = val;
        else if (key == "description") m.description = val;
        else if (key == "releases")
        {
            // game IDs or region names, separated by commas or spaces
            std::string id;
            for (char c : val + ",")
                if (c == ',' || c == ' ' || c == ';')
                {
                    if (!id.empty()) m.releases.push_back(id);
                    id.clear();
                }
                else
                    id += (char)toupper((unsigned char)c);
        }
    }
}

bool ReleaseMatches(const std::vector<std::string>& releases, const std::string& gameId)
{
    if (releases.empty() || gameId.empty()) return true;
    static const std::pair<const char*, const char*> names[] = {
        { "USA", "RENE8P" }, { "US", "RENE8P" }, { "NTSC-U", "RENE8P" }, { "EUROPE", "RENP8P" }, { "EUR", "RENP8P" },
        { "PAL", "RENP8P" }, { "JAPAN", "RENJ8P" }, { "JPN", "RENJ8P" }, { "NTSC-J", "RENJ8P" },
    };
    for (auto& r : releases)
    {
        if (r == gameId) return true;
        for (auto& [name, id] : names)
            if (r == name && gameId == id) return true;
    }
    return false;
}

// Where the mirror of files/ starts: <mod>/files if there is one, else the mod folder.
// A folder holding just one folder that has a mod.ini or files/ (an archive extracted
// with an extra level) is looked through.
fs::path ContentRoot(const fs::path& dir)
{
    std::error_code ec;
    fs::path d = dir;
    for (int depth = 0; depth < 3; depth++)
    {
        if (fs::is_directory(d / "files", ec)) return d / "files";
        if (fs::exists(d / "mod.ini", ec)) return d;
        fs::path only;
        int dirs = 0, files = 0;
        for (auto& e : fs::directory_iterator(d, ec))
        {
            if (e.is_directory(ec)) { dirs++; only = e.path(); }
            else files++;
        }
        if (dirs == 1 && files == 0 &&
            (fs::is_directory(only / "files", ec) || fs::exists(only / "mod.ini", ec)))
        {
            d = only;
            continue;
        }
        return d;
    }
    return d;
}

// files under root, relative and '/'-separated
std::vector<std::pair<std::string, fs::path>> ListFiles(const fs::path& root)
{
    std::vector<std::pair<std::string, fs::path>> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string rel = Utf8(it->path().lexically_relative(root));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        out.push_back({ rel, it->path() });
    }
    std::sort(out.begin(), out.end());
    return out;
}

// files a mod may carry besides game files (not reported as unknown)
bool IsModMetadata(const std::string& rel)
{
    std::string l = Lower(rel);
    size_t slash = l.rfind('/');
    std::string name = slash == std::string::npos ? l : l.substr(slash + 1);
    std::string ext = name.find('.') == std::string::npos ? "" : name.substr(name.rfind('.'));
    return name == "mod.ini" || name == "packed_files.bin" || ext == ".json" || ext == ".md" || ext == ".png" ||
           ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".webp" || name.rfind("readme", 0) == 0;
}

fs::path ExeDir()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}
}   // namespace

void Mods_SetDirOverride(const std::string& dir) { s_dirOverride = dir; }

std::vector<fs::path> Mods_Roots(const std::string& configuredDir)
{
    if (!s_dirOverride.empty()) return { fs::path(s_dirOverride) };
    if (!configuredDir.empty()) return { fs::path(configuredDir) };
    fs::path exe = ExeDir();
    return { exe / "mods", exe / "game" / "mods", exe / ".." / "game" / "mods" };
}

fs::path Mods_MainDir(const std::string& configuredDir)
{
    std::error_code ec;
    for (auto& r : Mods_Roots(configuredDir))
        if (fs::is_directory(r, ec)) return fs::weakly_canonical(r, ec);
    return Mods_Roots(configuredDir).front();
}

std::vector<ModInfo> Mods_Scan(const std::vector<fs::path>& roots, const std::vector<ModSetting>& settings,
                               const std::string& gameId)
{
    std::vector<ModInfo> found;
    std::set<std::string> seen;   // folder names, lower case: the first root wins
    std::error_code ec;
    for (auto& root : roots)
    {
        if (!fs::is_directory(root, ec)) continue;
        std::vector<fs::path> dirs;
        for (auto& e : fs::directory_iterator(root, ec))
            if (e.is_directory(ec)) dirs.push_back(e.path());
        std::sort(dirs.begin(), dirs.end());
        for (auto& d : dirs)
        {
            ModInfo m;
            m.folder = Utf8(d.filename());
            if (m.folder.empty() || m.folder[0] == '.' || !seen.insert(Lower(m.folder)).second) continue;
            m.dir = d;
            m.root = ContentRoot(d);
            for (fs::path ini : { d / "mod.ini", m.root / "mod.ini", m.root.parent_path() / "mod.ini" })
                if (fs::exists(ini, ec))
                {
                    ReadIni(ini, m);
                    break;
                }
            if (m.title.empty()) m.title = m.folder;
            m.forRelease = ReleaseMatches(m.releases, gameId);
            m.fileCount = ListFiles(m.root).size();
            found.push_back(std::move(m));
        }
    }
    // load order: the configured mods first, then the new ones (already sorted by name)
    std::vector<ModInfo> mods;
    std::vector<bool> used(found.size());
    for (auto& s : settings)
        for (size_t i = 0; i < found.size(); i++)
            if (!used[i] && Lower(found[i].folder) == Lower(s.folder))
            {
                used[i] = true;
                found[i].enabled = s.enabled;
                mods.push_back(found[i]);
                break;
            }
    for (size_t i = 0; i < found.size(); i++)
        if (!used[i]) mods.push_back(found[i]);
    return mods;
}

std::vector<ModSetting> Mods_Settings(const std::vector<ModInfo>& mods)
{
    std::vector<ModSetting> s;
    for (auto& m : mods) s.push_back({ m.folder, m.enabled });
    return s;
}

std::map<std::string, ModOverride> Mods_BuildOverlay(const std::vector<ModInfo>& mods,
                                                     const std::vector<std::string>& discFiles)
{
    // disc paths by lower case (Windows file names and the mods' are case-insensitive)
    std::map<std::string, std::string> disc;
    for (auto& f : discFiles) disc.emplace(Lower(f), f);

    std::map<std::string, ModOverride> result;
    std::map<std::string, std::vector<size_t>> claimed;   // disc path -> mods that have it
    for (size_t i = 0; i < mods.size(); i++)
    {
        const ModInfo& m = mods[i];
        if (!m.enabled) continue;
        if (!m.forRelease)
        {
            LOG_INFO("mods: {} is not for this release (mod.ini releases), not loaded", m.title);
            continue;
        }
        int replaced = 0;
        std::vector<std::string> unknown;
        for (auto& [rel, host] : ListFiles(m.root))
        {
            auto d = disc.find(Lower(rel));
            if (d == disc.end())
            {
                if (!IsModMetadata(rel)) unknown.push_back(rel);
                continue;
            }
            std::error_code ec;
            uint64_t size = fs::file_size(host, ec);
            if (ec || size > 0xFFFFFFFFull)
            {
                LOG_WARN("mods: {}: cannot use {}", m.title, Utf8(host));
                continue;
            }
            replaced++;
            claimed[d->second].push_back(i);
            if (result.emplace(d->second, ModOverride{ host, size, i }).second)
                LOG_INFO("mods: {} replaces {} ({} bytes)", m.title, d->second, size);
        }
        LOG_INFO("mods: {} ({}) loaded: {} game files", m.title, Utf8(m.root), replaced);
        if (!unknown.empty())
        {
            std::string list;
            for (size_t k = 0; k < unknown.size() && k < 5; k++) list += (k ? ", " : "") + unknown[k];
            if (unknown.size() > 5) list += ", ...";
            LOG_INFO("mods: {}: {} files are not game files (ignored): {}", m.title, unknown.size(), list);
        }
    }
    for (auto& [path, who] : claimed)
        if (who.size() > 1)
        {
            std::string others;
            for (size_t k = 1; k < who.size(); k++) others += (k > 1 ? ", " : "") + mods[who[k]].title;
            LOG_WARN("mods: {} is replaced by more than one mod; {} wins (load order) over {}", path,
                     mods[who[0]].title, others);
        }
    return result;
}

void Mods_SetLoaded(std::vector<ModInfo> mods) { s_loaded = std::move(mods); }
const std::vector<ModInfo>& Mods_Loaded() { return s_loaded; }
