#pragma once
// MD5 of a file, to check mod downloads against GameBanana's checksums (Windows: CNG;
// Linux: portable code).
#include <cstdint>
#include <filesystem>

bool Md5_File(const std::filesystem::path& file, uint8_t out[16]);
