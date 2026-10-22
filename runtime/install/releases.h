#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

// The releases of the game the recompilation supports. One exe plays all of them: the
// installed disc's game ID picks the recompiled code (runtime/region.cpp).

struct KnownFile
{
    const char* path;
    uint64_t size;
    const char* sha1;   // hex
};

struct GameRelease
{
    const char* gameId;     // RENE8P
    const char* name;       // USA
    uint64_t titleId;       // 00010000 + the first four characters of the game ID
    // system settings of a console of that region (setting.txt, SYSCONF IPL.SADR)
    const char* area;
    const char* model;
    const char* code;
    const char* video;
    const char* game;
    uint8_t country;
    // every installed file (tools/gen_file_manifest.py): path, size, SHA-1
    const KnownFile* files;
    size_t fileCount;
};

extern const GameRelease kReleases[3];
// "USA, Europe or Japan"
extern const char* const kReleaseNames;

const GameRelease* Release_Find(const std::string& gameId);
// Game ID of installed game data (sys/boot.bin), or an empty string
std::string Release_InstalledId(const std::filesystem::path& data);
