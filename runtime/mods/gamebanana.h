#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The game's mods on GameBanana (gamebanana.com/games/6511), read from its public API
// (apiv11). Blocking calls: use them from a worker thread.

constexpr uint32_t kGameBananaGameId = 6511;   // Sonic and the Black Knight

struct GbFile
{
    uint64_t id = 0;
    std::string name;          // archive file name
    std::string url;           // download link
    std::string md5;
    std::string version;
    std::string description;
    uint64_t size = 0;
    int64_t added = 0;         // Unix time
    bool archived = false;     // an older file the author kept
    bool clean = true;         // GameBanana's file check did not flag it
};

struct GbMod
{
    uint64_t id = 0;
    std::string model;         // "Mod", "Sound"
    std::string name, author, category, version;
    std::string summary;       // one line
    std::string text;          // the description, as plain text (details)
    std::string url;           // profile page
    std::string thumb;         // small preview image
    std::vector<std::string> images;   // screenshots, large (details)
    int64_t added = 0, modified = 0;   // Unix time
    uint32_t likes = 0, views = 0, downloads = 0;
    std::vector<GbFile> files;         // details
    bool details = false;              // Gb_FetchDetails succeeded
};

// Every mod and sound of the game that has files, newest first.
bool Gb_FetchList(std::vector<GbMod>& out, std::string& error);
// Description, screenshots and files of one mod.
bool Gb_FetchDetails(GbMod& mod, std::string& error);
// The API answers as GbMod (exposed for tests)
bool Gb_ParseList(const std::string& json, std::vector<GbMod>& out, bool& complete);
bool Gb_ParseDetails(const std::string& json, GbMod& mod);
// GameBanana descriptions are HTML: text with line breaks and entities decoded
std::string Gb_HtmlToText(const std::string& html);
