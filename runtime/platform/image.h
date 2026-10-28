#pragma once
// PNG files for texture packs and texture dumps (Windows: WIC; Linux: SDL3).
#include <cstdint>
#include <filesystem>
#include <vector>

// Prepares the codecs; the other calls fail until it has been called.
void Image_Init();
// RGBA8 pixels, rows top to bottom.
bool Image_LoadPNG(const std::filesystem::path& path, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba);
bool Image_SavePNG(const std::filesystem::path& path, uint32_t width, uint32_t height, const uint32_t* rgba);
