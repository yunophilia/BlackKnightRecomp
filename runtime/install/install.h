#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

// Installs the game data from the user's disc image (ISO, RVZ, WIA, WBFS, CISO, GCZ, ...)
// using nod. The output is the layout DolphinTool's "extract -g" produces, which the
// runtime reads:
//   <dest>/sys/{boot.bin,bi2.bin,apploader.img,main.dol,fst.bin}
//   <dest>/files/...                      (the data partition's file system)
//   <dest>/{ticket,tmd,cert,h3}.bin
//   <dest>/disc/{header,region}.bin
//   <dest>/disc/unencrypted.bin           (first 0x50000 bytes of the disc, for raw reads)
// so the disc image is not needed afterwards. Everything is written to <dest>.partial
// first and renamed at the end; it is removed again if the install fails or is canceled.

struct InstallProgress
{
    std::atomic<uint64_t> done{ 0 };     // bytes written
    std::atomic<uint64_t> total{ 0 };    // known once the file system has been read
    std::atomic<bool> cancel{ false };
};

enum class InstallResult
{
    Ok,
    OpenFailed,       // not a readable disc image
    WrongGame,        // not RENE8P
    WrongVersion,     // RENE8P, but main.dol differs from the one this build was made from
    WriteFailed,
    Canceled,
};

// Game ID of the image, or an empty string if it cannot be opened
std::string Install_GameId(const std::filesystem::path& image);

InstallResult Install_Run(const std::filesystem::path& image, const std::filesystem::path& dest,
                          InstallProgress& progress, std::string& error);

// File dialog filter description of the supported image types
extern const wchar_t* const kInstallImageFilter;
