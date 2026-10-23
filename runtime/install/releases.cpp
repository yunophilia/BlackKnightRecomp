#include "releases.h"

#include <cstdio>
#include <iterator>

namespace
{
constexpr KnownFile kFilesRENE8P[] = {
#include "files_rene8p.inc"
};
constexpr KnownFile kFilesRENP8P[] = {
#include "files_renp8p.inc"
};
constexpr KnownFile kFilesRENJ8P[] = {
#include "files_renj8p.inc"
};

constexpr uint64_t TitleId(const char* id)
{
    return 0x00010000'00000000ull | ((uint64_t)(uint8_t)id[0] << 24) | ((uint64_t)(uint8_t)id[1] << 16) |
           ((uint64_t)(uint8_t)id[2] << 8) | (uint8_t)id[3];
}
}   // namespace

const GameRelease kReleases[3] = {
    { "RENE8P", "USA", TitleId("RENE"), "USA", "RVL-001(USA)", "LU", "NTSC", "US", 0x31,
      kFilesRENE8P, std::size(kFilesRENE8P) },
    { "RENP8P", "Europe", TitleId("RENP"), "EUR", "RVL-001(EUR)", "LEH", "PAL", "EU", 0x6E,
      kFilesRENP8P, std::size(kFilesRENP8P) },
    { "RENJ8P", "Japan", TitleId("RENJ"), "JPN", "RVL-001(JPN)", "LJ", "NTSC", "JP", 0x01,
      kFilesRENJ8P, std::size(kFilesRENJ8P) },
};

const char* const kReleaseNames = "USA, Europe or Japan";

const GameRelease* Release_Find(const std::string& gameId)
{
    for (auto& r : kReleases)
        if (gameId == r.gameId) return &r;
    return nullptr;
}

std::string Release_InstalledId(const std::filesystem::path& data)
{
    char id[6] = {};
    FILE* f = _wfopen((data / "sys" / "boot.bin").c_str(), L"rb");
    if (!f) return {};
    size_t n = fread(id, 1, 6, f);
    fclose(f);
    return n == 6 ? std::string(id, 6) : std::string();
}
