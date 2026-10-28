#pragma once
// PNG files for texture packs and texture dumps (Windows: WIC; Linux: SDL3), and the mod
// browser's web images (Windows: WIC; Linux: the system's libpng and libjpeg).
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

// Prepares the codecs; the other calls fail until it has been called.
void Image_Init();
// RGBA8 pixels, rows top to bottom.
bool Image_LoadPNG(const std::filesystem::path& path, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba);
bool Image_SavePNG(const std::filesystem::path& path, uint32_t width, uint32_t height, const uint32_t* rgba);
// A PNG or JPEG file in memory (Windows also GIF, WebP and others) as RGBA8, at most
// 4096 x 4096, checked before the pixels are allocated; false for anything else, a damaged
// file or (Linux) missing libraries. Usable from any thread, without Image_Init.
bool Image_Decode(const void* data, size_t size, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba);
