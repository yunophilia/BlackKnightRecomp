#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// File-replacement mods: each folder in mods/ is a mod whose files mirror the disc's
// files/ tree (adx/, event/, sound/, *.one, ...), either at the mod's top level or under a
// files/ subfolder (Storybook Mod Manager mods use the top level). An optional mod.ini
// names it. When the game opens a file, the first enabled mod in load order that has it
// wins, otherwise the installed original is read; the installed data is never changed
// (runtime/ios/disc.cpp builds the overlay).

struct ModInfo
{
    std::string folder;              // folder name: the mod's key in config.ini
    std::filesystem::path dir;       // the mod folder
    std::filesystem::path root;      // where the mirror of files/ starts (dir or dir/files)
    std::string title, author, version, description;   // mod.ini, title defaults to folder
    std::vector<std::string> releases;   // mod.ini "releases" (game IDs); empty: all releases
    bool enabled = true;
    bool forRelease = true;          // applies to the installed release
    size_t fileCount = 0;            // files under root (mod.ini and the like included)
};

// Load order and on/off state as stored in config.ini [Mods]
struct ModSetting
{
    std::string folder;
    bool enabled = true;
};

// Folders searched for mods: the configured one (config.ini mods_dir, or --mods for one
// run), else mods/ next to the exe, game/mods and ../game/mods (the repository layout),
// like the texture pack folders.
std::vector<std::filesystem::path> Mods_Roots(const std::string& configuredDir);
void Mods_SetDirOverride(const std::string& dir);
// The folder the UI opens and the README names (the first existing root, else <exe>/mods).
std::filesystem::path Mods_MainDir(const std::string& configuredDir);

// Every mod in the roots, in load order: mods listed in the settings first (in that order,
// with their state), then new ones by folder name, enabled. gameId (may be empty) sets
// forRelease from mod.ini "releases".
std::vector<ModInfo> Mods_Scan(const std::vector<std::filesystem::path>& roots, const std::vector<ModSetting>& settings,
                               const std::string& gameId);
std::vector<ModSetting> Mods_Settings(const std::vector<ModInfo>& mods);

// Which mod file replaces which disc file. discFiles are the paths of the disc's files
// relative to files/ ('/'-separated, as in the FST); the result is keyed by the same
// strings. Only enabled mods for this release take part; the first in order wins. Logs
// what every mod replaces, files two mods both replace, and mod files the disc does not
// have.
struct ModOverride
{
    std::filesystem::path host;
    uint64_t size = 0;
    size_t mod = 0;     // index into mods
};
std::map<std::string, ModOverride> Mods_BuildOverlay(const std::vector<ModInfo>& mods,
                                                     const std::vector<std::string>& discFiles);

// The mods loaded into the running game (set at start-up, shown by the in-game settings)
void Mods_SetLoaded(std::vector<ModInfo> mods);
const std::vector<ModInfo>& Mods_Loaded();
