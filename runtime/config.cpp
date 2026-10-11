#include "config.h"
#include "log.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

Config g_config;

static const char* kDefaultIni =
    "; BlackKnightRecomp settings\n"
    "\n"
    "[Video]\n"
    "; 30 = original speed and pacing, 60 = doubled frame rate\n"
    "frame_rate = 30\n"
    "; internal resolution multiplier (1 = 640x528, 3 = 1920x1584)\n"
    "internal_scale = 3\n"
    "window_width = 1280\n"
    "window_height = 720\n"
    "; F11 or Alt+Enter toggles fullscreen at runtime\n"
    "fullscreen = false\n"
    "vsync = false\n"
    "widescreen = true\n"
    "; Dolphin-format HD texture packs: <dir>/RENE8P/**/tex1_*.png|dds. Empty dir searches\n"
    "; textures/ next to the exe, game/textures and Documents/Dolphin Emulator/Load/Textures\n"
    "texture_pack = true\n"
    "texture_pack_dir =\n"
    "\n"
    "[System]\n"
    "; english, german, french, spanish, italian, dutch, japanese\n"
    "language = english\n"
    "\n"
    "[Input]\n"
    "rumble = true\n"
    "\n"
    "[Paths]\n"
    "; leave empty to search next to the executable (game/extracted/DATA, game/*.iso)\n"
    "data =\n"
    "iso =\n";

static std::string Trim(std::string s)
{
    auto ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && ws(s[i])) i++;
    return s.substr(i);
}

static std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

static bool ParseBool(const std::string& v)
{
    std::string l = Lower(v);
    return l == "1" || l == "true" || l == "yes" || l == "on";
}

static int ParseLanguage(const std::string& v)
{
    static const char* names[] = { "japanese", "english", "german", "french", "spanish", "italian", "dutch" };
    std::string l = Lower(v);
    for (int i = 0; i < 7; i++)
        if (l == names[i]) return i;
    if (!l.empty() && isdigit((unsigned char)l[0])) return std::clamp(atoi(l.c_str()), 0, 6);
    LOG_WARN("config: unknown language '{}', using english", v);
    return 1;
}

void Config_Load(const std::string& path)
{
    std::ifstream f(path);
    if (!f)
    {
        if (FILE* o = fopen(path.c_str(), "wb"))
        {
            fputs(kDefaultIni, o);
            fclose(o);
        }
        return;
    }
    std::string line, section;
    while (std::getline(f, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[')
        {
            section = Lower(Trim(line.substr(1, line.find(']') - 1)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Lower(Trim(line.substr(0, eq)));
        std::string val = Trim(line.substr(eq + 1));
        Config& c = g_config;
        if (section == "video")
        {
            if (key == "frame_rate") c.frameRate = atoi(val.c_str()) >= 60 ? 60 : 30;
            else if (key == "internal_scale") c.internalScale = std::clamp(atoi(val.c_str()), 1, 8);
            else if (key == "window_width") c.windowWidth = std::max(320, atoi(val.c_str()));
            else if (key == "window_height") c.windowHeight = std::max(240, atoi(val.c_str()));
            else if (key == "fullscreen") c.fullscreen = ParseBool(val);
            else if (key == "vsync") c.vsync = ParseBool(val);
            else if (key == "widescreen") c.widescreen = ParseBool(val);
            else if (key == "texture_pack") c.texturePack = ParseBool(val);
            else if (key == "texture_pack_dir") c.texturePackDir = val;
        }
        else if (section == "system")
        {
            if (key == "language") c.language = ParseLanguage(val);
        }
        else if (section == "input")
        {
            if (key == "rumble") c.rumble = ParseBool(val);
        }
        else if (section == "paths")
        {
            if (key == "data") c.dataDir = val;
            else if (key == "iso") c.isoPath = val;
        }
    }
}
