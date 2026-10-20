#pragma once
#include <string>

// User settings loaded from config.ini next to the executable (created with
// defaults on first run). Command line options override them.
struct Config
{
    // [Video]
    int frameRate = 30;          // 30 (original) or 60
    int internalScale = 3;       // EFB resolution multiplier (1 = 640x528)
    int windowWidth = 1280;
    int windowHeight = 720;
    bool fullscreen = false;
    bool vsync = false;
    bool widescreen = true;
    int anisotropy = 1;          // forced anisotropic filtering (1 = as the game sets it)
    bool asyncShaders = true;    // compile new shaders in the background (no hitches)
    bool gpuThread = true;       // GX processing/rendering on its own thread
    bool texturePack = true;     // load Dolphin-format custom textures if present
    std::string texturePackDir;  // empty: <exe>/textures, game/textures, Dolphin's Load/Textures
    // [System]
    int language = 1;                // Wii IPL.LNG: 0 ja, 1 en, 2 de, 3 fr, 4 es, 5 it, 6 nl
    bool pauseOnFocusLoss = false;   // freeze the game while the window is in the background
    // [Input]
    bool rumble = true;
    bool remoteSpeaker = true;   // play Wii Remote speaker sounds on the PC
    // button bindings, action -> controller button name (A, B, X, Y, LB, RB, LT, RT,
    // START, BACK, LS, RS, UP, DOWN, LEFT, RIGHT or NONE); see config.cpp for actions.
    // Wii "-" is unbound by default: the game does not use it, and BACK (the View button)
    // opens the settings overlay.
    std::string bind[12] = { "A", "X", "B", "Y", "RB", "LB", "START", "NONE", "LS", "RS", "RT", "LT" };
    // [Paths]
    std::string dataDir;
    std::string isoPath;
};

extern Config g_config;

extern const char* const kBindActions[12];

// Loads config.ini (writing the defaults if it does not exist) and remembers its path.
void Config_Load(const std::string& path);
// Writes the current g_config back to config.ini (with comments).
bool Config_Save();
const std::string& Config_Path();
std::string Config_Text(const Config& c);
