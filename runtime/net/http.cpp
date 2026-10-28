#include "http.h"
#include "../platform/http.h"

#include <cstdio>

bool Http_Get(const std::string& url, std::string& body, std::string& error, HttpProgress* progress)
{
    body.clear();
    return Http_Fetch(url, [&](const uint8_t* p, size_t n) { body.append((const char*)p, n); return true; }, error,
                      progress, kHttpMaxGet);
}

bool Http_Download(const std::string& url, const std::filesystem::path& file, std::string& error, HttpProgress* progress,
                   uint64_t maxBytes)
{
    if (_strnicmp(url.c_str(), "https://", 8) != 0)
    {
        error = "not an https address: " + url;
        return false;
    }
    std::filesystem::path part = file;
    part += ".part";
    FILE* f = _wfopen(part.c_str(), L"wb");
    if (!f)
    {
        error = "cannot create " + part.string();
        return false;
    }
    bool ok = Http_Fetch(url, [&](const uint8_t* p, size_t n) { return fwrite(p, 1, n, f) == n; }, error, progress,
                         maxBytes);
    ok = fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) std::filesystem::rename(part, file, ec);
    if (!ok || ec)
    {
        std::filesystem::remove(part, ec);
        if (ok) error = "cannot write " + file.string();
        return false;
    }
    return true;
}
