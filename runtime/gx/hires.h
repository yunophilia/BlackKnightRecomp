#pragma once
#include <cstdint>
#include <string>

struct ID3D11ShaderResourceView;

// Dolphin-compatible custom texture packs: files named
//   tex1_<w>x<h>[_m]_<xxh64 of data>[_<xxh64 of used palette>|_$]_<fmt>.png/.dds
// anywhere under <pack dir>/RENE8P (the layout of Dolphin's Load/Textures).
void HiRes_Init(const std::string& configuredDir);
bool HiRes_Enabled();

// Looks up a replacement for a texture about to be decoded. `data` covers the base
// level, `tlut` the 512-byte-aligned TMEM palette (CI formats). Returns an SRV with
// a reference owned by the caller, or nullptr.
ID3D11ShaderResourceView* HiRes_Find(const uint8_t* data, uint32_t dataSize, const uint8_t* tlut, uint32_t tlutSize,
                                     uint32_t fmt, uint32_t w, uint32_t h, bool mipmapped);

// --dump-textures: writes every decoded texture as <exe>/dump/textures/RENE8P/<name>.png
extern bool g_dumpTextures;
void HiRes_Dump(const uint8_t* data, uint32_t dataSize, const uint8_t* tlut, uint32_t tlutSize, uint32_t fmt, uint32_t w, uint32_t h,
                bool mipmapped, const uint32_t* rgba);
