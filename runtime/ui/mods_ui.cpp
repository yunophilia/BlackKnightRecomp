#include "mods_ui.h"
#include "ui_image.h"
#include "../config.h"
#include "../log.h"
#include "../mods/gamebanana.h"
#include "../mods/mod_install.h"
#include "../mods/mods.h"
#include "../net/http.h"

#include <imgui.h>
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace fs = std::filesystem;

namespace
{
// ---------------------------------------------------------------- installed mods
std::vector<ModInfo> s_mods;               // the mods folder as last scanned, in load order
std::vector<InstalledMod> s_records;       // mods/gamebanana.json
bool s_scanned = false;
std::string s_release;                     // game ID
fs::path s_dataDir;
std::string s_help;                        // hovered item's description

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

std::string Utf8(const fs::path& p)
{
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path ModsDir() { return Mods_MainDir(g_config.modsDir); }

void Scan()
{
    s_mods = Mods_Scan(Mods_Roots(g_config.modsDir), g_config.mods, s_release);
    s_records = ModInstall_Records(ModsDir());
    s_scanned = true;
}

InstallTargets Targets()
{
    InstallTargets t;
    t.modsDir = ModsDir();
    t.texturesDir = ModInstall_TexturesDir(g_config.texturePackDir);
    t.gameId = s_release.empty() ? "RENE8P" : s_release;
    if (!s_dataDir.empty()) t.discFiles = ModInstall_DiscFiles(s_dataDir);
    return t;
}

const InstalledMod* RecordOfFolder(const std::string& folder)
{
    for (auto& r : s_records)
        for (auto& m : r.mods)
            if (Lower(m) == Lower(folder)) return &r;
    return nullptr;
}

const InstalledMod* RecordOf(uint64_t modId)
{
    for (auto& r : s_records)
        if (r.modId == modId) return &r;
    return nullptr;
}

// mods that load with the current settings differ from the ones the game started with
bool ChangedSinceStart()
{
    std::vector<std::string> now, loaded;
    for (auto& m : s_mods)
        if (m.enabled && m.forRelease) now.push_back(m.folder);
    for (auto& m : Mods_Loaded())
        if (m.enabled && m.forRelease) loaded.push_back(m.folder);
    return now != loaded;
}

void Hover(const std::string& text)
{
    if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) s_help = text;
}

std::string Date(int64_t t)
{
    if (!t) return "?";
    std::time_t tt = (std::time_t)t;
    std::tm tm{};
    gmtime_s(&tm, &tt);
    return std::format("{:04}-{:02}-{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

std::string Size(uint64_t b)
{
    if (b >= 1000000) return std::format("{:.1f} MB", b / 1e6);
    return std::format("{} KB", (b + 999) / 1000);
}

void OpenUrl(const std::string& url)
{
    if (url.rfind("https://", 0) != 0) return;   // only web pages
    int n = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, w.data(), n);
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool Md5Matches(const fs::path& file, const std::string& expected)
{
    if (expected.size() != 32) return true;   // GameBanana gave none
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) return false;
    BCRYPT_HASH_HANDLE h = nullptr;
    BCryptCreateHash(BCRYPT_MD5_ALG_HANDLE, &h, nullptr, 0, nullptr, 0, 0);
    std::vector<uint8_t> buf(1 << 20);
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) BCryptHashData(h, buf.data(), (ULONG)n, 0);
    fclose(f);
    uint8_t d[16];
    BCryptFinishHash(h, d, 16, 0);
    BCryptDestroyHash(h);
    std::string hex;
    for (uint8_t b : d) hex += std::format("{:02x}", b);
    return hex == Lower(expected);
}

// ---------------------------------------------------------------- GameBanana catalog
// Workers fetch outside the lock and publish under it; the UI draws under it.
struct Catalog
{
    std::mutex mutex;
    enum State { None, Loading, Ready, Failed } state = None;
    std::vector<GbMod> mods;
    std::string error;
    std::set<uint64_t> detailsPending;
};
std::shared_ptr<Catalog> s_cat = std::make_shared<Catalog>();

GbMod* FindMod(Catalog& c, uint64_t id)
{
    for (auto& m : c.mods)
        if (m.id == id) return &m;
    return nullptr;
}

// details of these mods (ids), one after the other; caller holds the lock
void FetchDetails(std::vector<uint64_t> ids)
{
    std::shared_ptr<Catalog> cat = s_cat;
    std::vector<GbMod> todo;
    for (uint64_t id : ids)
        if (GbMod* m = FindMod(*cat, id); m && !m->details && cat->detailsPending.insert(id).second) todo.push_back(*m);
    if (todo.empty()) return;
    std::thread([cat, todo]() mutable {
        for (auto& m : todo)
        {
            std::string error;
            bool ok = Gb_FetchDetails(m, error);
            if (!ok) LOG_WARN("mods: GameBanana details of {}: {}", m.id, error);
            std::lock_guard lk(cat->mutex);
            cat->detailsPending.erase(m.id);
            if (GbMod* dst = FindMod(*cat, m.id); dst && ok) *dst = m;
        }
    }).detach();
}

void LoadCatalog()
{
    std::shared_ptr<Catalog> cat = s_cat;
    {
        std::lock_guard lk(cat->mutex);
        if (cat->state == Catalog::Loading) return;
        cat->state = Catalog::Loading;
        cat->error.clear();
    }
    // installed mods: their details tell whether an update is out
    std::vector<uint64_t> installed;
    for (auto& r : s_records) installed.push_back(r.modId);
    std::thread([cat, installed] {
        std::vector<GbMod> mods;
        std::string error;
        bool ok = Gb_FetchList(mods, error);
        std::lock_guard lk(cat->mutex);
        if (!ok)
        {
            cat->state = Catalog::Failed;
            cat->error = error;
            LOG_WARN("mods: GameBanana: {}", error);
            return;
        }
        // keep details already loaded
        for (auto& m : mods)
            if (GbMod* old = FindMod(*cat, m.id); old && old->details) m = *old;
        cat->mods = std::move(mods);
        cat->state = Catalog::Ready;
        FetchDetails(installed);
    }).detach();
}

// the newest file of a mod (details loaded), or null
const GbFile* LatestFile(const GbMod& m)
{
    for (auto& f : m.files)
        if (!f.archived) return &f;
    return m.files.empty() ? nullptr : &m.files[0];
}

bool UpdateAvailable(const GbMod& m, const InstalledMod& r)
{
    const GbFile* f = LatestFile(m);
    return f && f->id != r.fileId && f->added > r.fileAdded;
}

// ---------------------------------------------------------------- install / remove jobs
struct Job
{
    std::mutex mutex;
    bool running = false, done = false;
    std::string what;            // "Downloading Silver..."
    HttpProgress progress;
    InstallResult result;
};
std::shared_ptr<Job> s_job;
std::string s_lastMessage;
bool s_lastOk = true;

void StartInstall(const GbMod& m, const GbFile& f)
{
    if (s_job && s_job->running) return;
    auto job = std::make_shared<Job>();
    job->running = true;
    job->what = std::format("Downloading {} ({})", m.name, f.name);
    s_job = job;
    InstallTargets t = Targets();
    InstallSource src;
    src.modId = m.id;
    src.fileId = f.id;
    src.name = m.name;
    src.author = m.author;
    src.version = f.version.empty() ? m.version : f.version;
    src.fileName = f.name;
    src.fileAdded = f.added;
    std::string url = f.url, md5 = f.md5, fileName = f.name;
    // at most twice the size GameBanana lists (the checksum catches the rest)
    uint64_t maxBytes = f.size ? std::min(kHttpMaxDownload, f.size * 2 + (1 << 20)) : kHttpMaxDownload;
    LOG_INFO("mods: installing {} from GameBanana ({}, {} bytes)", m.name, f.name, f.size);
    std::thread([job, t, src, url, md5, fileName, maxBytes] {
        std::error_code ec;
        fs::path dir = t.modsDir / ".download";
        fs::create_directories(dir, ec);
        std::string safe;
        for (char c : fileName) safe += isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' ? c : '_';
        fs::path file = dir / std::format("{}_{}", src.fileId, safe);
        InstallResult res;
        std::string error;
        if (!Http_Download(url, file, error, &job->progress, maxBytes))
            res.message = std::format("Download of {} failed: {}", src.name, error);
        else if (!Md5Matches(file, md5))
            res.message = std::format("The download of {} is damaged (checksum differs); try again.", src.name);
        else
        {
            {
                std::lock_guard lk(job->mutex);
                job->what = std::format("Installing {}", src.name);
            }
            res = ModInstall_Archive(file, src, t, &job->progress.cancel);
        }
        fs::remove(file, ec);
        fs::remove(dir, ec);   // when empty
        std::lock_guard lk(job->mutex);
        job->result = res;
        job->running = false;
        job->done = true;
    }).detach();
}

// a finished job: rescan; true when the mod setup changed
bool PollJob()
{
    std::shared_ptr<Job> job = s_job;
    if (!job) return false;
    InstallResult res;
    {
        std::lock_guard lk(job->mutex);
        if (!job->done) return false;
        res = job->result;
    }
    s_job.reset();
    s_lastOk = res.ok;
    s_lastMessage = res.message;
    for (auto& n : res.notes) s_lastMessage += "\n" + n;
    if (!res.ok) return false;
    // variants of one mod: only the first is on
    for (size_t i = 1; i < res.modFolders.size(); i++)
    {
        std::erase_if(g_config.mods, [&](const ModSetting& m) { return Lower(m.folder) == Lower(res.modFolders[i]); });
        g_config.mods.push_back({ res.modFolders[i], false });
    }
    Scan();
    g_config.mods = Mods_Settings(s_mods);
    return true;
}

// ---------------------------------------------------------------- popups
std::string s_removeFolder;     // asked: remove this mod folder ...
uint64_t s_removeModId = 0;     // ... or everything this GameBanana install added?
std::string s_removeName;
bool s_openRemove = false;

void AskRemove(const std::string& folder, uint64_t modId, const std::string& name)
{
    s_removeFolder = folder;
    s_removeModId = modId;
    s_removeName = name;
    s_openRemove = true;
}

// ---------------------------------------------------------------- installed tab
bool InstalledTab(SettingsHost host)
{
    bool changed = false;
    fs::path dir = ModsDir();
    ImGui::TextWrapped("File-replacement mods (skins, HUD changes, ...): one folder per mod in %s. "
                       "The installed game files are never changed.", Utf8(dir).c_str());
    if (ImGui::Button("Open mods folder"))
    {
        std::error_code ec;
        fs::create_directories(dir, ec);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    Hover("Open the mods folder in Explorer (created if it does not exist).");
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) Scan();
    Hover("Look for new or removed mods.");
    if (!s_records.empty())
    {
        ImGui::SameLine();
        if (ImGui::Button("Check for updates")) LoadCatalog();
        Hover("Ask GameBanana whether the mods installed from there have new versions.");
    }
    if (host == SettingsHost::InGame && ChangedSinceStart())
        ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_CheckMark], "Restart the game to load the changed mods.");

    ImGui::SeparatorText("Load order (a mod higher in the list wins when two replace the same file)");
    if (s_mods.empty())
        ImGui::TextDisabled("No mods yet. Get some from GameBanana in the next tab, or put mod folders in the mods folder.");
    int move = -1, moveBy = 0;
    std::lock_guard lk(s_cat->mutex);
    if (!s_mods.empty() && ImGui::BeginTable("mods", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Mod", ImGuiTableColumnFlags_WidthStretch, 3);
        ImGui::TableSetupColumn("Author", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)s_mods.size(); i++)
        {
            ModInfo& m = s_mods[i];
            const InstalledMod* rec = RecordOfFolder(m.folder);
            const GbMod* gb = rec ? FindMod(*s_cat, rec->modId) : nullptr;
            ImGui::PushID(m.folder.c_str());   // focus follows a mod when it moves
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            std::string label = m.title + (host == SettingsHost::InGame ? " (restart)" : "");
            if (ImGui::Checkbox(label.c_str(), &m.enabled)) changed = true;
            std::string help = m.description.empty() ? m.title : m.title + ": " + m.description;
            help += " (folder " + m.folder + ", " + std::to_string(m.fileCount) + " files";
            if (rec) help += ", from GameBanana";
            help += ")";
            if (!m.forRelease) help += " Not for this release of the game (mod.ini releases), so it is not loaded.";
            Hover(help);
            if (!m.forRelease)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(not for this release)");
            }
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(m.author.c_str());
            ImGui::TableNextColumn();
            if (gb && gb->details && UpdateAvailable(*gb, *rec))
            {
                const GbFile* f = LatestFile(*gb);
                std::string up = "Update" + (f->version.empty() ? std::string() : " to " + f->version);
                ImGui::BeginDisabled(s_job != nullptr);
                if (ImGui::Button(up.c_str())) StartInstall(*gb, *f);
                ImGui::EndDisabled();
                Hover(std::format("{} has a new file on GameBanana ({}, {}). The old files go to the Recycle Bin.",
                                  gb->name, f->name, Date(f->added)));
            }
            else
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(m.version.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(i == 0);
            if (ImGui::ArrowButton("up", ImGuiDir_Up)) { move = i; moveBy = -1; }
            ImGui::EndDisabled();
            Hover("Move up: load earlier (wins over the mods below).");
            ImGui::SameLine();
            ImGui::BeginDisabled(i + 1 == (int)s_mods.size());
            if (ImGui::ArrowButton("down", ImGuiDir_Down)) { move = i; moveBy = 1; }
            ImGui::EndDisabled();
            Hover("Move down: load later.");
            ImGui::SameLine();
            ImGui::BeginDisabled(s_job != nullptr);
            if (ImGui::Button("Remove")) AskRemove(m.folder, 0, m.title);
            ImGui::EndDisabled();
            Hover("Remove this mod (its folder goes to the Recycle Bin).");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (move >= 0)
    {
        std::swap(s_mods[move], s_mods[move + moveBy]);
        changed = true;
    }
    // texture packs installed from GameBanana
    bool header = false;
    for (auto& r : s_records)
    {
        if (r.textures.empty() || !r.mods.empty()) continue;
        if (!header) ImGui::SeparatorText("Texture packs from GameBanana");
        header = true;
        ImGui::PushID((int)r.modId);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s", r.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(textures/%s)", r.textures[0].c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(s_job != nullptr);
        if (ImGui::Button("Remove")) AskRemove("", r.modId, r.name);
        ImGui::EndDisabled();
        Hover("Remove this texture pack (to the Recycle Bin).");
        ImGui::PopID();
    }
    if (changed) g_config.mods = Mods_Settings(s_mods);
    return changed;
}

// ---------------------------------------------------------------- browse tab
uint64_t s_selected = 0;
int s_image = 0;            // screenshot shown
int s_category = 0, s_sort = 0;
char s_search[128] = "";
size_t s_fileChoice = 0;
bool s_openFlagged = false;     // the file was flagged by GameBanana's check: confirm
bool s_focusActions = false;    // move keyboard/controller focus to the selected mod's buttons

bool Matches(const GbMod& m, const std::string& category)
{
    if (!category.empty() && m.category != category) return false;
    std::string q = Lower(s_search);
    if (q.empty()) return true;
    return Lower(m.name).find(q) != std::string::npos || Lower(m.author).find(q) != std::string::npos;
}

void DrawList(Catalog& c, const std::vector<std::string>& categories)
{
    std::string category = s_category > 0 && s_category < (int)categories.size() ? categories[s_category] : "";
    std::vector<const GbMod*> shown;
    for (auto& m : c.mods)
        if (Matches(m, category)) shown.push_back(&m);
    std::stable_sort(shown.begin(), shown.end(), [](const GbMod* a, const GbMod* b) {
        switch (s_sort)
        {
        case 1: return a->added > b->added;
        case 2: return a->views > b->views;
        case 3: return Lower(a->name) < Lower(b->name);
        default: return a->modified > b->modified;
        }
    });
    float rowH = ImGui::GetTextLineHeight() * 2.6f;
    float thumbW = rowH * 16 / 9;
    if (shown.empty()) ImGui::TextDisabled("No mods match.");
    for (const GbMod* m : shown)
    {
        ImGui::PushID((int)m->id);
        bool sel = m->id == s_selected;
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool pressed = ImGui::Selectable("##mod", sel, 0, ImVec2(0, rowH));
        // keyboard/controller: moving onto an entry shows it, pressing it goes to its buttons
        if (pressed && ImGui::GetIO().NavVisible) s_focusActions = true;
        if (pressed || (ImGui::IsItemFocused() && !sel && ImGui::GetIO().NavVisible))
        {
            s_selected = m->id;
            s_image = 0;
            s_fileChoice = 0;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImTextureID tex = UiImage_Get(m->thumb);
        if (tex) dl->AddImage(ImTextureRef(tex), p, ImVec2(p.x + thumbW, p.y + rowH));
        else dl->AddRectFilled(p, ImVec2(p.x + thumbW, p.y + rowH), ImGui::GetColorU32(ImGuiCol_FrameBg));
        float x = p.x + thumbW + ImGui::GetStyle().ItemSpacing.x;
        const InstalledMod* rec = RecordOf(m->id);
        std::string title = m->name;
        if (rec) title += m->details && UpdateAvailable(*m, *rec) ? "  [update]" : "  [installed]";
        dl->PushClipRect(ImVec2(x, p.y), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), p.y + rowH), true);
        dl->AddText(ImVec2(x, p.y + 2), ImGui::GetColorU32(ImGuiCol_Text), title.c_str());
        std::string sub = m->author + "  -  " + m->category;
        dl->AddText(ImVec2(x, p.y + ImGui::GetTextLineHeight() * 1.3f), ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str());
        dl->PopClipRect();
        ImGui::PopID();
    }
}

void DrawDetails(Catalog& c, SettingsHost host)
{
    GbMod* m = FindMod(c, s_selected);
    if (!m)
    {
        ImGui::TextDisabled("Pick a mod on the left.");
        return;
    }
    if (!m->details && !c.detailsPending.count(m->id)) FetchDetails({ m->id });
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextWrapped("%s", m->name.c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("by %s  -  %s%s%s", m->author.c_str(), m->category.c_str(),
                        m->version.empty() ? "" : "  -  version ", m->version.c_str());
    ImGui::TextDisabled("updated %s  -  %u views%s", Date(m->modified).c_str(), m->views,
                        m->details ? std::format("  -  {} likes  -  {} downloads", m->likes, m->downloads).c_str() : "");

    // actions first (next to the list, for controller navigation), then the screenshots
    const InstalledMod* rec = RecordOf(m->id);
    if (!m->details)
        ImGui::TextDisabled(c.detailsPending.count(m->id) ? "Loading..." : "Cannot load the details.");
    else
    {
        std::vector<const GbFile*> files;
        for (auto& f : m->files)
            if (!f.archived) files.push_back(&f);
        if (files.empty())
            for (auto& f : m->files) files.push_back(&f);
        if (files.empty())
            ImGui::TextDisabled("No downloads.");
        else
        {
            s_fileChoice = std::min(s_fileChoice, files.size() - 1);
            if (files.size() > 1)
            {
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                std::string cur = std::format("{} ({})", files[s_fileChoice]->name, Size(files[s_fileChoice]->size));
                if (ImGui::BeginCombo("##file", cur.c_str()))
                {
                    for (size_t i = 0; i < files.size(); i++)
                    {
                        std::string item = std::format("{} ({})", files[i]->name, Size(files[i]->size));
                        if (ImGui::Selectable(item.c_str(), i == s_fileChoice)) s_fileChoice = i;
                    }
                    ImGui::EndCombo();
                }
                Hover("This mod has several downloads (often a Storybook Mod Manager version and a manual one). "
                      "Either usually works; the first is the newest.");
            }
            const GbFile& f = *files[s_fileChoice];
            if (!f.description.empty()) ImGui::TextDisabled("%s", f.description.c_str());
            bool update = rec && UpdateAvailable(*m, *rec);
            const char* label = !rec ? "Install" : update ? "Update" : "Reinstall";
            if (s_focusActions)
            {
                ImGui::SetKeyboardFocusHere();
                s_focusActions = false;
            }
            ImGui::BeginDisabled(s_job != nullptr);
            if (ImGui::Button(std::format("{} ({})", label, Size(f.size)).c_str()))
            {
                if (!f.clean) s_openFlagged = true;
                else StartInstall(*m, f);
            }
            ImGui::EndDisabled();
            Hover(host == SettingsHost::InGame ? "Download and install it; the game then needs a restart."
                                               : "Download and install it into the mods folder.");
            if (rec)
            {
                ImGui::SameLine();
                ImGui::BeginDisabled(s_job != nullptr);
                if (ImGui::Button("Remove")) AskRemove("", rec->modId, m->name);
                Hover("Remove everything this mod installed (to the Recycle Bin).");
                ImGui::EndDisabled();
            }
            if (!f.clean) ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.3f, 1), "GameBanana's file check flagged this file.");
            if (s_openFlagged)
            {
                ImGui::OpenPopup("Install a flagged file?");
                s_openFlagged = false;
            }
            if (ImGui::BeginPopupModal("Install a flagged file?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("GameBanana's automatic check flagged %s.", f.name.c_str());
                ImGui::TextDisabled("Only install it if you trust the mod's author.");
                if (ImGui::Button("Install anyway")) { StartInstall(*m, f); ImGui::CloseCurrentPopup(); }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Open page")) OpenUrl(m->url);
    Hover("Open the mod's GameBanana page in your web browser.");
    ImGui::Spacing();

    // screenshot
    if (!m->images.empty())
    {
        s_image = std::clamp(s_image, 0, (int)m->images.size() - 1);
        float w = ImGui::GetContentRegionAvail().x;
        ImVec2 size;
        ImTextureID tex = UiImage_Get(m->images[s_image], &size);
        float h = tex && size.x > 0 ? std::min(w * size.y / size.x, w * 0.6f) : w * 0.45f;
        float iw = tex && size.y > 0 ? h * size.x / size.y : w;
        if (tex) ImGui::Image(ImTextureRef(tex), ImVec2(iw, h));
        else ImGui::Dummy(ImVec2(w, h));
        if (m->images.size() > 1)
        {
            if (ImGui::ArrowButton("prev", ImGuiDir_Left)) s_image = (s_image + (int)m->images.size() - 1) % (int)m->images.size();
            ImGui::SameLine();
            ImGui::Text("%d / %d", s_image + 1, (int)m->images.size());
            ImGui::SameLine();
            if (ImGui::ArrowButton("next", ImGuiDir_Right)) s_image = (s_image + 1) % (int)m->images.size();
        }
    }

    if (!m->summary.empty()) ImGui::TextWrapped("%s", m->summary.c_str());
    ImGui::Separator();
    if (!m->text.empty()) ImGui::TextWrapped("%s", m->text.c_str());
}

void BrowseTab(SettingsHost host)
{
    std::shared_ptr<Catalog> cat = s_cat;
    bool load = false;
    {
        std::lock_guard lk(cat->mutex);
        load = cat->state == Catalog::None;
    }
    if (load) LoadCatalog();
    load = false;
    struct Reload { bool& load; ~Reload() { if (load) LoadCatalog(); } } reload{ load };   // after the lock below
    std::lock_guard lk(cat->mutex);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Mods by the community from gamebanana.com. Only install what you trust; "
                       "code mods and Dolphin settings cannot work on the PC version.");
    ImGui::PopStyleColor();
    if (cat->state == Catalog::Loading && cat->mods.empty())
    {
        ImGui::Text("Loading the mod list from GameBanana...");
        return;
    }
    if (cat->state == Catalog::Failed)
    {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1), "Cannot load the mod list: %s", cat->error.c_str());
        if (ImGui::Button("Try again")) load = true;
        return;
    }
    // filters
    std::vector<std::string> categories = { "All categories" };
    for (auto& m : cat->mods)
        if (std::find(categories.begin(), categories.end(), m.category) == categories.end() && !m.category.empty())
            categories.push_back(m.category);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11);
    ImGui::InputTextWithHint("##search", "Search", s_search, sizeof(s_search));
    Hover("Find mods by name or author.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    s_category = std::clamp(s_category, 0, (int)categories.size() - 1);
    if (ImGui::BeginCombo("##category", categories[s_category].c_str()))
    {
        for (int i = 0; i < (int)categories.size(); i++)
            if (ImGui::Selectable(categories[i].c_str(), i == s_category)) s_category = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    static const char* sorts[] = { "Recently updated", "Newest", "Most viewed", "Name" };
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    ImGui::Combo("##sort", &s_sort, sorts, 4);
    ImGui::SameLine();
    if (ImGui::Button("Reload")) load = true;

    if (ImGui::BeginTable("browse", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, ImGui::GetContentRegionAvail().y)))
    {
        ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("details", ImGuiTableColumnFlags_WidthStretch, 0.58f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginChild("list", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
        DrawList(*cat, categories);
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::BeginChild("details", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
        DrawDetails(*cat, host);
        ImGui::EndChild();
        ImGui::EndTable();
    }
}

// test aids (UI screenshots, scripted install checks): BKR_UI_SHOT_MODS=browse opens the
// GameBanana tab, BKR_UI_SHOT_MODS_SELECT=<id> selects that mod, BKR_MODS_AUTOINSTALL=<id>
// installs its newest file once its details are loaded
void TestAids(bool& openBrowse)
{
    static const char* view = getenv("BKR_UI_SHOT_MODS");
    static const char* select = getenv("BKR_UI_SHOT_MODS_SELECT");
    static const char* install = getenv("BKR_MODS_AUTOINSTALL");
    static int frames = 0;
    openBrowse = view && !_stricmp(view, "browse") && frames++ < 2;
    if (select && !s_selected) s_selected = strtoull(select, nullptr, 10);
    static bool installed = false;
    if (install && !installed)
    {
        uint64_t id = strtoull(install, nullptr, 10);
        std::shared_ptr<Catalog> cat = s_cat;
        std::unique_lock lk(cat->mutex);
        if (cat->state == Catalog::None) { lk.unlock(); LoadCatalog(); return; }
        GbMod* m = FindMod(*cat, id);
        if (!m) return;
        if (!m->details) { if (!cat->detailsPending.count(id)) FetchDetails({ id }); return; }
        if (const GbFile* f = LatestFile(*m)) { StartInstall(*m, *f); installed = true; }
    }
}
}   // namespace

bool ModsUI_Draw(SettingsHost host, const char*& help)
{
    s_help.clear();
    if (!s_scanned) Scan();
    bool changed = PollJob();
    bool openBrowse = false;
    TestAids(openBrowse);
    // job status / last result
    if (s_job)
    {
        std::lock_guard lk(s_job->mutex);
        uint64_t total = s_job->progress.total, done = s_job->progress.done;
        float frac = total ? (float)((double)done / total) : 0.0f;
        std::string text = total ? std::format("{} / {}", Size(done), Size(total)) : Size(done);
        ImGui::ProgressBar(frac, ImVec2(ImGui::GetFontSize() * 12, 0), text.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(s_job->what.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) s_job->progress.cancel = true;
    }
    else if (!s_lastMessage.empty())
        ImGui::TextColored(s_lastOk ? ImVec4(0.45f, 0.80f, 0.40f, 1) : ImVec4(0.90f, 0.55f, 0.30f, 1), "%s",
                           s_lastMessage.c_str());

    if (ImGui::BeginTabBar("modsView"))
    {
        static int selected = 0;
        int pick = SettingsUI_PadTab(selected, 2, false);
        if (SettingsUI_TabItem("Installed", 0, pick))
        {
            selected = 0;
            changed |= InstalledTab(host);
            ImGui::EndTabItem();
        }
        if (SettingsUI_TabItem("Get mods (GameBanana)", 1, pick, openBrowse))
        {
            selected = 1;
            BrowseTab(host);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // remove confirmation
    if (s_openRemove)
    {
        ImGui::OpenPopup("Remove mod?");
        s_openRemove = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Remove mod?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Remove %s?", s_removeName.c_str());
        ImGui::TextDisabled("Its files go to the Recycle Bin.");
        if (ImGui::Button("Remove"))
        {
            std::string msg;
            s_lastOk = s_removeModId ? ModInstall_Remove(Targets(), s_removeModId, msg)
                                     : ModInstall_RemoveFolder(Targets(), s_removeFolder, msg);
            s_lastMessage = msg;
            Scan();
            g_config.mods = Mods_Settings(s_mods);
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    help = s_help.empty() ? nullptr : s_help.c_str();
    return changed;
}

void ModsUI_SetRelease(const std::string& gameId, const std::string& dataDir)
{
    s_release = gameId;
    s_dataDir = dataDir;
    Scan();
}

std::vector<std::string> ModsUI_ActiveMods()
{
    if (!s_scanned) Scan();
    std::vector<std::string> titles;
    for (auto& m : s_mods)
        if (m.enabled && m.forRelease) titles.push_back(m.title);
    return titles;
}
