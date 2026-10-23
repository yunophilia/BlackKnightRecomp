#include "config.h"
#include "log.h"
#include "platform/platform.h"

#include <algorithm>
#include <cstdio>
#include <format>
#include <fstream>
#include <sstream>

Config g_config;
static std::string s_path;

// order matches Config::bind
const char* const kBindActions[12] = { "jump", "slash", "wii_b", "shake_nunchuk", "guard", "nunchuk_c",
                                       "plus", "minus", "button_1", "button_2", "thrust", "pull_back" };

// order matches Config::keys
const char* const kKeyActions[21] = { "jump", "slash", "wii_b", "shake_nunchuk", "guard", "nunchuk_c", "plus", "minus",
                                      "button_1", "button_2", "thrust", "pull_back", "home", "dpad_up", "dpad_down",
                                      "dpad_left", "dpad_right", "move_up", "move_down", "move_left", "move_right" };

static const char* const kLanguages[] = { "japanese", "english", "german", "french", "spanish", "italian", "dutch" };

static const char* Bool(bool b) { return b ? "true" : "false"; }

// The whole file, with comments, for the given settings (written on first run and
// whenever the launcher or the in-game settings change something).
std::string Config_Text(const Config& c)
{
    std::string s;
    s += "; BlackKnightRecomp settings (also editable in the launcher and in game: View button / F1)\n\n";
    s += "[Video]\n";
    s += "; 30 = original speed and pacing, 60 = doubled frame rate\n";
    s += std::format("frame_rate = {}\n", c.frameRate);
    s += "; internal resolution multiplier (1 = 640x528, 3 = 1920x1584)\n";
    s += std::format("internal_scale = {}\n", c.internalScale);
    s += std::format("window_width = {}\nwindow_height = {}\n", c.windowWidth, c.windowHeight);
    s += "; F11 or Alt+Enter toggles fullscreen at runtime\n";
    s += std::format("fullscreen = {}\nvsync = {}\nwidescreen = {}\n", Bool(c.fullscreen), Bool(c.vsync), Bool(c.widescreen));
    s += "; force anisotropic texture filtering: 1 (off), 2, 4, 8, 16\n";
    s += std::format("anisotropy = {}\n", c.anisotropy);
    s += "; process graphics on a second CPU core (like Dolphin's dual core)\n";
    s += std::format("gpu_thread = {}\n", Bool(c.gpuThread));
    s += "; compile new shaders in the background: no hitches, objects may appear a frame late\n";
    s += std::format("async_shaders = {}\n", Bool(c.asyncShaders));
    s += "; Dolphin-format HD texture packs: <dir>/<game ID>/**/tex1_*.png|dds. Empty dir searches\n";
    s += "; textures/ next to the exe, game/textures and Documents/Dolphin Emulator/Load/Textures\n";
    s += std::format("texture_pack = {}\ntexture_pack_dir = {}\n\n", Bool(c.texturePack), c.texturePackDir);
    s += "[System]\n";
    s += "; english, german, french, spanish, italian, dutch, japanese\n";
    s += std::format("language = {}\n", kLanguages[std::clamp(c.language, 0, 6)]);
    s += "; freeze the game while its window is in the background\n";
    s += std::format("pause_on_focus_loss = {}\n\n", Bool(c.pauseOnFocusLoss));
    s += "[Input]\n";
    s += std::format("rumble = {}\n", Bool(c.rumble));
    s += "; play sounds the game sends to the Wii Remote speaker\n";
    s += std::format("remote_speaker = {}\n", Bool(c.remoteSpeaker));
    s += "; controller bindings: A B X Y LB RB LT RT START BACK LS RS UP DOWN LEFT RIGHT NONE\n";
    s += "; (BACK, the View button, opens the settings overlay; Back+Start together is HOME;\n";
    s += ";  the right stick also flicks slashes)\n";
    for (int i = 0; i < 12; i++) s += std::format("{} = {}\n", kBindActions[i], c.bind[i]);
    s += "\n[Keyboard]\n";
    s += "; keyboard and mouse bindings, comma-separated: A-Z 0-9 F2-F10 F12 SPACE ENTER BACKSPACE TAB\n";
    s += "; LSHIFT RSHIFT LCTRL RCTRL LALT RALT UP DOWN LEFT RIGHT INSERT DELETE HOME_KEY END PAGEUP\n";
    s += "; PAGEDOWN NUMPAD0-NUMPAD9 MOUSE1 (left) MOUSE2 (right) MOUSE3 (middle) MOUSE4 MOUSE5 NONE\n";
    s += "; (F1 opens the settings, F11 toggles fullscreen; the mouse also moves the pointer)\n";
    for (int i = 0; i < 21; i++) s += std::format("{} = {}\n", kKeyActions[i], c.keys[i]);
    s += "\n[Paths]\n";
    s += "; leave empty to search next to the executable (game/extracted/DATA, game/*.iso)\n";
    s += std::format("data = {}\niso = {}\n", c.dataDir, c.isoPath);
    return s;
}

const std::string& Config_Path() { return s_path; }

bool Config_Save()
{
    if (s_path.empty()) return false;
    // write-then-rename so a crash never leaves a truncated file
    std::string tmp = s_path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { LOG_WARN("config: cannot write {}", tmp); return false; }
    std::string text = Config_Text(g_config);
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = fclose(f) == 0 && ok;
    if (ok) ok = Platform_ReplaceFile(tmp, s_path);
    if (!ok) LOG_WARN("config: cannot save {}", s_path);
    return ok;
}

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
    std::string l = Lower(v);
    for (int i = 0; i < 7; i++)
        if (l == kLanguages[i]) return i;
    if (!l.empty() && isdigit((unsigned char)l[0])) return std::clamp(atoi(l.c_str()), 0, 6);
    LOG_WARN("config: unknown language '{}', using english", v);
    return 1;
}

void Config_Load(const std::string& path)
{
    s_path = path;
    std::ifstream f(path);
    if (!f)
    {
        Config_Save();   // first run: write the defaults
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
            else if (key == "gpu_thread") c.gpuThread = ParseBool(val);
            else if (key == "async_shaders") c.asyncShaders = ParseBool(val);
            else if (key == "anisotropy") c.anisotropy = std::clamp(atoi(val.c_str()), 1, 16);
            else if (key == "texture_pack_dir") c.texturePackDir = val;
        }
        else if (section == "system")
        {
            if (key == "language") c.language = ParseLanguage(val);
            else if (key == "pause_on_focus_loss") c.pauseOnFocusLoss = ParseBool(val);
        }
        else if (section == "input")
        {
            if (key == "rumble") c.rumble = ParseBool(val);
            if (key == "remote_speaker") c.remoteSpeaker = ParseBool(val);
            for (int i = 0; i < 12; i++)
                if (key == kBindActions[i]) c.bind[i] = val;
        }
        else if (section == "keyboard")
        {
            for (int i = 0; i < 21; i++)
                if (key == kKeyActions[i]) c.keys[i] = val;
        }
        else if (section == "paths")
        {
            if (key == "data") c.dataDir = val;
            else if (key == "iso") c.isoPath = val;
        }
    }
}
