#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

// Integrity checks of installed game data against the known RENE8P file list
// (rene8p_files.inc: path, size and SHA-1 of every file the installer writes).

enum class DataStatus
{
    Ok,
    Missing,   // nothing installed (no sys/main.dol)
    Corrupt,   // files missing, truncated, or with the wrong content
};

struct VerifyProgress
{
    std::atomic<uint64_t> done{ 0 };    // bytes hashed
    std::atomic<uint64_t> total{ 0 };
    std::atomic<bool> cancel{ false };
};

// Every file present with the right size, and main.dol's SHA-1 (a fraction of a second).
DataStatus Install_QuickCheck(const std::filesystem::path& data, std::string& problem);
// SHA-1 of every file (reads the whole 3.3 GB). Returns Ok if canceled before a problem.
DataStatus Install_FullCheck(const std::filesystem::path& data, VerifyProgress& progress, std::string& problem);
