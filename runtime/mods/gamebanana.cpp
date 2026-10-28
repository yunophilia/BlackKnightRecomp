#include "gamebanana.h"
#include "../log.h"
#include "../net/http.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>

using json = nlohmann::json;

namespace
{
const char* const kApi = "https://gamebanana.com/apiv11";

std::string Str(const json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number()) return std::to_string(it->get<int64_t>());
    return {};
}

int64_t Num(const json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end()) return 0;
    if (it->is_number()) return it->get<int64_t>();
    if (it->is_string()) return atoll(it->get<std::string>().c_str());
    return 0;
}

bool Bool(const json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

// preview media: _aImages[{_sBaseUrl, _sFile, _sFile220, _sFile530, ...}]
std::string Image(const json& img, const char* sizeKey)
{
    std::string base = Str(img, "_sBaseUrl"), file = Str(img, sizeKey);
    if (file.empty()) file = Str(img, "_sFile");
    return base.empty() || file.empty() ? std::string() : base + "/" + file;
}

void ReadCommon(const json& r, GbMod& m)
{
    m.id = (uint64_t)Num(r, "_idRow");
    m.model = Str(r, "_sModelName");
    m.name = Str(r, "_sName");
    m.url = Str(r, "_sProfileUrl");
    m.version = Str(r, "_sVersion");
    m.added = Num(r, "_tsDateAdded");
    m.modified = Num(r, "_tsDateModified");
    if (int64_t u = Num(r, "_tsDateUpdated")) m.modified = std::max(m.modified, u);
    m.views = (uint32_t)Num(r, "_nViewCount");
    if (r.contains("_nLikeCount")) m.likes = (uint32_t)Num(r, "_nLikeCount");
    if (r.contains("_nDownloadCount")) m.downloads = (uint32_t)Num(r, "_nDownloadCount");
    if (auto s = r.find("_aSubmitter"); s != r.end() && s->is_object()) m.author = Str(*s, "_sName");
    for (const char* key : { "_aCategory", "_aRootCategory" })
        if (auto c = r.find(key); c != r.end() && c->is_object() && m.category.empty()) m.category = Str(*c, "_sName");
    if (auto p = r.find("_aPreviewMedia"); p != r.end() && p->is_object())
        if (auto imgs = p->find("_aImages"); imgs != p->end() && imgs->is_array() && !imgs->empty())
        {
            m.thumb = Image((*imgs)[0], "_sFile220");
            m.images.clear();
            for (auto& img : *imgs) m.images.push_back(Image(img, "_sFile530"));
        }
}

bool Parse(const std::string& text, json& j)
{
    j = json::parse(text, nullptr, false);
    return !j.is_discarded() && j.is_object();
}
}   // namespace

std::string Gb_HtmlToText(const std::string& html)
{
    std::string out;
    for (size_t i = 0; i < html.size();)
    {
        char c = html[i];
        if (c == '<')
        {
            size_t end = html.find('>', i);
            if (end == std::string::npos) break;
            std::string tag = html.substr(i + 1, end - i - 1);
            std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char ch) { return (char)tolower(ch); });
            std::string name = tag.substr(0, tag.find_first_of(" /\t\n", 1));   // "/p", "br/"
            if (name == "br" || name == "/p" || name == "/div" || name == "/h1" || name == "/h2" || name == "/h3" ||
                name == "/ul" || name == "/ol" || name == "hr")
                out += '\n';
            else if (name == "li")
                out += "\n- ";
            i = end + 1;
        }
        else if (c == '&')
        {
            size_t end = html.find(';', i);
            std::string ent = end == std::string::npos || end - i > 10 ? "" : html.substr(i + 1, end - i - 1);
            uint32_t cp = 0;
            if (ent == "amp") cp = '&';
            else if (ent == "lt") cp = '<';
            else if (ent == "gt") cp = '>';
            else if (ent == "quot") cp = '"';
            else if (ent == "apos") cp = '\'';
            else if (ent == "nbsp") cp = ' ';
            else if (ent.size() > 1 && ent[0] == '#')
                cp = (uint32_t)(ent[1] == 'x' || ent[1] == 'X' ? strtoul(ent.c_str() + 2, nullptr, 16) : strtoul(ent.c_str() + 1, nullptr, 10));
            if (!cp)
            {
                out += c;
                i++;
                continue;
            }
            // UTF-8
            if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | (cp >> 6 & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
            else { out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | (cp >> 12 & 0x3F)); out += (char)(0x80 | (cp >> 6 & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
            i = end + 1;
        }
        else
        {
            out += c == '\r' || c == '\n' ? ' ' : c;
            i++;
        }
    }
    // collapse blank runs: at most one empty line, no spaces at line starts
    std::string tidy;
    int newlines = 0;
    for (char c : out)
    {
        if (c == '\n')
        {
            while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
            if (++newlines <= 2 && !tidy.empty()) tidy += '\n';
            continue;
        }
        if (c == ' ' && (tidy.empty() || tidy.back() == '\n' || tidy.back() == ' ')) continue;
        newlines = 0;
        tidy += c;
    }
    while (!tidy.empty() && (tidy.back() == '\n' || tidy.back() == ' ')) tidy.pop_back();
    return tidy;
}

bool Gb_ParseList(const std::string& text, std::vector<GbMod>& out, bool& complete)
{
    json j;
    if (!Parse(text, j) || !j.contains("_aRecords") || !j["_aRecords"].is_array()) return false;
    complete = true;
    if (auto md = j.find("_aMetadata"); md != j.end() && md->is_object()) complete = Bool(*md, "_bIsComplete");
    for (auto& r : j["_aRecords"])
    {
        if (!r.is_object() || !Bool(r, "_bHasFiles")) continue;
        std::string model = Str(r, "_sModelName");
        if (model != "Mod" && model != "Sound") continue;   // not tools, requests, questions, tutorials
        GbMod m;
        ReadCommon(r, m);
        if (m.id) out.push_back(std::move(m));
    }
    return true;
}

bool Gb_ParseDetails(const std::string& text, GbMod& m)
{
    json j;
    if (!Parse(text, j) || Num(j, "_idRow") == 0) return false;
    std::string model = m.model;
    ReadCommon(j, m);
    if (m.model.empty()) m.model = model;
    m.summary = Str(j, "_sDescription");
    m.text = Gb_HtmlToText(Str(j, "_sText"));
    m.files.clear();
    if (auto fs = j.find("_aFiles"); fs != j.end() && fs->is_array())
        for (auto& f : *fs)
        {
            GbFile g;
            g.id = (uint64_t)Num(f, "_idRow");
            g.name = Str(f, "_sFile");
            g.url = Str(f, "_sDownloadUrl");
            g.md5 = Str(f, "_sMd5Checksum");
            g.version = Str(f, "_sVersion");
            g.description = Str(f, "_sDescription");
            g.size = (uint64_t)Num(f, "_nFilesize");
            g.added = Num(f, "_tsDateAdded");
            g.archived = Bool(f, "_bIsArchived");
            std::string analysis = Str(f, "_sAnalysisResult"), av = Str(f, "_sAvResult");
            g.clean = (analysis.empty() || analysis == "ok") && (av.empty() || av == "clean");
            if (g.id && !g.url.empty()) m.files.push_back(std::move(g));
        }
    // newest file first, older (archived) ones last
    std::stable_sort(m.files.begin(), m.files.end(), [](const GbFile& a, const GbFile& b) {
        if (a.archived != b.archived) return !a.archived;
        return a.added > b.added;
    });
    m.details = true;
    return true;
}

bool Gb_FetchList(std::vector<GbMod>& out, std::string& error)
{
    out.clear();
    for (int page = 1; page <= 20; page++)
    {
        std::string body;
        std::string url = std::format("{}/Game/{}/Subfeed?_nPage={}&_sSort=default", kApi, kGameBananaGameId, page);
        if (!Http_Get(url, body, error)) return false;
        bool complete = true;
        if (!Gb_ParseList(body, out, complete))
        {
            error = "GameBanana sent an answer this version does not understand";
            return false;
        }
        if (complete) break;
    }
    std::stable_sort(out.begin(), out.end(), [](const GbMod& a, const GbMod& b) { return a.modified > b.modified; });
    LOG_INFO("mods: GameBanana lists {} mods", out.size());
    return true;
}

bool Gb_FetchDetails(GbMod& m, std::string& error)
{
    std::string body;
    std::string model = m.model.empty() ? "Mod" : m.model;
    if (!Http_Get(std::format("{}/{}/{}/ProfilePage", kApi, model, m.id), body, error)) return false;
    if (!Gb_ParseDetails(body, m))
    {
        error = "GameBanana sent an answer this version does not understand";
        return false;
    }
    return true;
}
