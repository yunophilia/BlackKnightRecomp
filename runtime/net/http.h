#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

// HTTPS downloads for the mod browser (WinHTTP; redirects are followed). Blocking: call
// them from a worker thread.

struct HttpProgress
{
    std::atomic<uint64_t> done{ 0 };
    std::atomic<uint64_t> total{ 0 };   // 0 while unknown
    std::atomic<bool> cancel{ false };
};

// The response body of a GET; false with a message on failure, a non-200 status or cancel.
bool Http_Get(const std::string& url, std::string& body, std::string& error, HttpProgress* progress = nullptr);
// Same, into a file (written to <file>.part, then renamed).
bool Http_Download(const std::string& url, const std::filesystem::path& file, std::string& error,
                   HttpProgress* progress = nullptr);
