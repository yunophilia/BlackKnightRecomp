#include "http.h"
#include "../log.h"
#include "../version.h"

#include <cstdio>
#include <format>
#include <functional>
#include <vector>
#include <windows.h>
#include <winhttp.h>

namespace
{
std::wstring Wide(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

struct Handle
{
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};

// GET url, passing the body to sink in pieces; false on failure (error set)
bool Fetch(const std::string& url, const std::function<bool(const uint8_t*, size_t)>& sink, std::string& error,
           HttpProgress* progress, uint64_t maxBytes)
{
    std::wstring wurl = Wide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[4096];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc))
    {
        error = "bad address " + url;
        return false;
    }
    std::wstring target = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) +
                          (uc.lpszExtraInfo ? std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength) : L"");
    std::wstring agent = L"BlackKnightRecomp/" + Wide(g_version);
    Handle session{ WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0) };
    if (!session.h)   // older Windows (and Wine) without the automatic proxy mode
        session.h = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h)
    {
        error = "cannot use the network (WinHTTP)";
        return false;
    }
    WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);
    Handle conn{ WinHttpConnect(session.h, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0) };
    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    Handle req{ conn.h ? WinHttpOpenRequest(conn.h, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0)
                       : nullptr };
    // never follow a redirect from https to plain http (WinHTTP's default, made explicit)
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (req.h) WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    if (!req.h || !WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr))
    {
        error = std::format("cannot reach {} (error {})", std::string(url.substr(0, url.find('/', 8))), GetLastError());
        return false;
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status != 200)
    {
        error = std::format("{} answered with status {}", url, status);
        return false;
    }
    uint64_t length = 0;
    {
        wchar_t buf[32];
        DWORD n = sizeof(buf);
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, buf, &n,
                                WINHTTP_NO_HEADER_INDEX))
            length = _wcstoui64(buf, nullptr, 10);
    }
    auto tooLarge = [&] { error = std::format("{} is larger than {} MB", url, maxBytes >> 20); return false; };
    if (length > maxBytes) return tooLarge();
    if (progress) progress->total = length;
    uint64_t received = 0;
    std::vector<uint8_t> chunk(1 << 16);
    for (;;)
    {
        if (progress && progress->cancel)
        {
            error = "canceled";
            return false;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req.h, chunk.data(), (DWORD)chunk.size(), &got))
        {
            error = std::format("download from {} interrupted (error {})", url, GetLastError());
            return false;
        }
        if (got == 0) break;
        received += got;
        if (received > maxBytes) return tooLarge();
        if (!sink(chunk.data(), got))
        {
            error = "cannot write the download";
            return false;
        }
        if (progress) progress->done += got;
    }
    return true;
}
}   // namespace

bool Http_Get(const std::string& url, std::string& body, std::string& error, HttpProgress* progress)
{
    body.clear();
    return Fetch(url, [&](const uint8_t* p, size_t n) { body.append((const char*)p, n); return true; }, error, progress,
                 kHttpMaxGet);
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
    bool ok = Fetch(url, [&](const uint8_t* p, size_t n) { return fwrite(p, 1, n, f) == n; }, error, progress, maxBytes);
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
