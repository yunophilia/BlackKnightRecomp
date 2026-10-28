#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Installs a downloaded mod archive (zip, rar, 7z): unpacks it, finds what it holds and
// puts each part where the game loads it:
//  * game files (mirroring the disc's files/): the mod's folder goes to mods/ (mods.h),
//  * Dolphin-format textures (tex1_*.png/.dds): textures/<game ID>/<mod>/ (gx/hires.cpp).
// Code patches (main.dol, .dol/.elf, xdelta/BPS/IPS) cannot work with the recompiled game:
// such archives are refused. Dolphin settings and Gecko/Action Replay codes are skipped with
// a note. What was installed is recorded in mods/gamebanana.ini for updates and removal.

struct InstallSource
{
    uint64_t modId = 0, fileId = 0;     // GameBanana ids (0: a local archive)
    std::string name;                   // the mod's name (folder name when the archive has none)
    std::string author;                 // written to mod.ini when the mod has none
    std::string version, fileName;
    int64_t fileAdded = 0;              // Unix time of the installed file
};

struct InstallTargets
{
    std::filesystem::path modsDir, texturesDir;
    std::string gameId;                 // installed release, the texture folder when a pack has none
    std::vector<std::string> discFiles; // paths of the disc's files relative to files/
    bool recycle = true;                // replaced and removed files go to the Recycle Bin (tests: deleted)
};

struct InstallResult
{
    bool ok = false;
    std::string message;                // what happened, for the user
    std::vector<std::string> modFolders;   // folders in mods/ (more than one: the archive had variants)
    std::vector<std::string> textureFolders;   // folders in textures/ (relative)
    std::vector<std::string> notes;     // skipped parts
};

InstallResult ModInstall_Archive(const std::filesystem::path& archive, const InstallSource& source,
                                 const InstallTargets& targets, std::atomic<bool>* cancel = nullptr);

// mods/gamebanana.ini: one section per installed GameBanana mod
struct InstalledMod
{
    uint64_t modId = 0, fileId = 0;
    std::string name, version, fileName, date;
    int64_t fileAdded = 0;
    std::vector<std::string> mods;       // folders in mods/
    std::vector<std::string> textures;   // folders in textures/ (relative)
};
std::vector<InstalledMod> ModInstall_Records(const std::filesystem::path& modsDir);
// Removes what a record installed (to the Recycle Bin) and the record.
bool ModInstall_Remove(const InstallTargets& targets, uint64_t modId, std::string& message);
// Removes one mod folder (to the Recycle Bin) and drops it from the records.
bool ModInstall_RemoveFolder(const InstallTargets& targets, const std::string& folder, std::string& message);

// textures/ next to the exe or game/textures (the configured texture_pack_dir wins)
std::filesystem::path ModInstall_TexturesDir(const std::string& configuredDir);
// The disc's file paths (relative to files/) from an installed sys/fst.bin
std::vector<std::string> ModInstall_DiscFiles(const std::filesystem::path& dataDir);
