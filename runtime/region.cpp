#include "region.h"
#include "game_addrs.h"
#include "log.h"

#include <algorithm>
#include <format>

extern const PPCRegionCode* const g_ppcRegionCode[];   // generated/ppc_regions.cpp

namespace
{
// one table per release, columns of game_addrs.inc in kReleases order
#define GAME_ADDR(name, kind, usa, eur, jpn) usa,
constexpr uint32_t kAddrsRENE8P[] = {
#include "game_addrs.inc"
};
#undef GAME_ADDR
#define GAME_ADDR(name, kind, usa, eur, jpn) eur,
constexpr uint32_t kAddrsRENP8P[] = {
#include "game_addrs.inc"
};
#undef GAME_ADDR
#define GAME_ADDR(name, kind, usa, eur, jpn) jpn,
constexpr uint32_t kAddrsRENJ8P[] = {
#include "game_addrs.inc"
};
#undef GAME_ADDR
const uint32_t* const kAddrs[] = { kAddrsRENE8P, kAddrsRENP8P, kAddrsRENJ8P };
static_assert(std::size(kAddrs) == std::size(kReleases));

const GameRelease* s_release = nullptr;
const PPCRegionCode* s_code = nullptr;
}   // namespace

const uint32_t* g_gameAddrs = nullptr;
const PPCFunc* g_ppcOriginals = nullptr;

bool Region_Select(const std::string& gameId, std::string& error)
{
    const GameRelease* rel = Release_Find(gameId);
    if (!rel)
    {
        error = std::format("the installed game ({}) is not Sonic and the Black Knight ({})", gameId, kReleaseNames);
        return false;
    }
    const PPCRegionCode* code = nullptr;
    std::string built;
    for (const PPCRegionCode* const* c = g_ppcRegionCode; *c; c++)
    {
        if (gameId == (*c)->gameId) code = *c;
        built += std::string(built.empty() ? "" : ", ") + (*c)->gameId;
    }
    if (!code)
    {
        error = std::format("this build does not contain the code of the {} release ({}); it was built for {}",
                            rel->name, gameId, built);
        return false;
    }
    if (code->addrCount != (uint32_t)GA::Count)
    {
        error = "the recompiled code is out of date (runtime/game_addrs.inc changed): run tools/build_generated.py";
        return false;
    }
    s_release = rel;
    s_code = code;
    g_ppcRelease = code->release;
    std::copy_n(code->deltas, g_ppcDeltaCount, g_ppcDelta);
    g_gameAddrs = kAddrs[rel - kReleases];
    g_ppcOriginals = code->originals;
    LOG_INFO("game release: {} ({})", rel->name, rel->gameId);
    return true;
}

const GameRelease& Region_Current() { return *s_release; }
const PPCRegionCode& Region_Code() { return *s_code; }

std::string Region_TitleDataDir()
{
    return std::format("/title/{:08x}/{:08x}/data", (uint32_t)(s_release->titleId >> 32), (uint32_t)s_release->titleId);
}
