// HTTPS through the system's libcurl (every desktop distribution and SteamOS have it), loaded
// with dlopen when the mod browser first needs it: the build needs no curl headers, and the
// game runs without it (the mod browser then reports that it cannot use the network). The few
// libcurl names used are declared here; their values are part of libcurl's stable ABI.
#include "../http.h"
#include "../../log.h"
#include "../../version.h"

#include <dlfcn.h>

#include <format>
#include <functional>
#include <mutex>
#include <type_traits>

namespace
{
using CURL = void;
using CurlCode = int;
using CurlOffT = long long;
using WriteFn = size_t (*)(char*, size_t, size_t, void*);
using XferFn = int (*)(void*, CurlOffT, CurlOffT, CurlOffT, CurlOffT);

constexpr int kOptWriteData = 10001, kOptUrl = 10002, kOptErrorBuffer = 10010, kOptWriteFunction = 20011,
              kOptUserAgent = 10018, kOptLowSpeedLimit = 19, kOptLowSpeedTime = 20, kOptNoProgress = 43,
              kOptFollowLocation = 52, kOptMaxRedirs = 68, kOptConnectTimeout = 78, kOptNoSignal = 99,
              kOptXferInfoData = 10057, kOptXferInfoFunction = 20219, kOptSslVerifyPeer = 64, kOptSslVerifyHost = 81,
              kOptProtocols = 181, kOptRedirProtocols = 182, kOptMaxFileSizeLarge = 30117,
              kOptProtocolsStr = 10318, kOptRedirProtocolsStr = 10319;
constexpr long kProtoHttps = 1 << 1;
constexpr int kInfoResponseCode = 0x200000 + 2;
constexpr long kGlobalDefault = 3;
constexpr CurlCode kWriteError = 23, kAbortedByCallback = 42, kFileSizeExceeded = 63;
constexpr size_t kErrorSize = 256;

struct Curl
{
    CurlCode (*global_init)(long);
    CURL* (*easy_init)();
    CurlCode (*easy_setopt)(CURL*, int, ...);
    CurlCode (*easy_perform)(CURL*);
    CurlCode (*easy_getinfo)(CURL*, int, ...);
    void (*easy_cleanup)(CURL*);
    const char* (*easy_strerror)(CurlCode);
};

const Curl* Load()
{
    static Curl c;
    static bool ok = false;
    static std::once_flag once;
    std::call_once(once, [] {
        void* lib = nullptr;
        for (const char* name : { "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so" })
            if ((lib = dlopen(name, RTLD_NOW | RTLD_LOCAL))) break;
        if (!lib)
        {
            LOG_WARN("http: libcurl not found ({})", dlerror());
            return;
        }
        auto sym = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            return fn != nullptr;
        };
        ok = sym(c.global_init, "curl_global_init") && sym(c.easy_init, "curl_easy_init") &&
             sym(c.easy_setopt, "curl_easy_setopt") && sym(c.easy_perform, "curl_easy_perform") &&
             sym(c.easy_getinfo, "curl_easy_getinfo") && sym(c.easy_cleanup, "curl_easy_cleanup") &&
             sym(c.easy_strerror, "curl_easy_strerror") && c.global_init(kGlobalDefault) == 0;
        if (!ok) LOG_WARN("http: libcurl is unusable");
    });
    return ok ? &c : nullptr;
}

struct Transfer
{
    const Curl* curl;
    CURL* h;
    const std::function<bool(const uint8_t*, size_t)>* sink;
    HttpProgress* progress;
    uint64_t maxBytes;
    uint64_t received = 0;
    long status = 0;
    bool sinkFailed = false;
    bool tooLarge = false;
};

size_t OnData(char* p, size_t size, size_t n, void* user)
{
    auto& t = *(Transfer*)user;
    if (!t.status) t.curl->easy_getinfo(t.h, kInfoResponseCode, &t.status);
    if (t.status != 200) return 0;   // the body of an error page
    if (t.progress && t.progress->cancel) return 0;
    t.received += size * n;
    if (t.received > t.maxBytes)
    {
        t.tooLarge = true;
        return 0;
    }
    if (!(*t.sink)((const uint8_t*)p, size * n))
    {
        t.sinkFailed = true;
        return 0;
    }
    if (t.progress) t.progress->done += size * n;
    return size * n;
}

int OnProgress(void* user, CurlOffT total, CurlOffT, CurlOffT, CurlOffT)
{
    auto& t = *(Transfer*)user;
    if (t.progress && total > 0) t.progress->total = (uint64_t)total;
    return t.progress && t.progress->cancel ? 1 : 0;
}
}   // namespace

bool Http_Fetch(const std::string& url, const std::function<bool(const uint8_t*, size_t)>& sink, std::string& error,
                HttpProgress* progress, uint64_t maxBytes)
{
    const Curl* c = Load();
    if (!c)
    {
        error = "cannot use the network (libcurl is not installed)";
        return false;
    }
    CURL* h = c->easy_init();
    if (!h)
    {
        error = "cannot use the network (libcurl)";
        return false;
    }
    Transfer t{ c, h, &sink, progress, maxBytes };
    char message[kErrorSize] = {};
    std::string agent = std::string("BlackKnightRecomp/") + g_version;
    c->easy_setopt(h, kOptUrl, url.c_str());
    c->easy_setopt(h, kOptUserAgent, agent.c_str());
    // https only, also for redirects (libcurl 7.85+ names the protocols; older ones take a mask)
    if (c->easy_setopt(h, kOptProtocolsStr, "https") != 0 || c->easy_setopt(h, kOptRedirProtocolsStr, "https") != 0)
    {
        c->easy_setopt(h, kOptProtocols, kProtoHttps);
        c->easy_setopt(h, kOptRedirProtocols, kProtoHttps);
    }
    c->easy_setopt(h, kOptSslVerifyPeer, 1L);   // libcurl's defaults, made explicit
    c->easy_setopt(h, kOptSslVerifyHost, 2L);
    c->easy_setopt(h, kOptMaxFileSizeLarge, (CurlOffT)maxBytes);   // by Content-Length, before the body
    c->easy_setopt(h, kOptFollowLocation, 1L);
    c->easy_setopt(h, kOptMaxRedirs, 10L);
    c->easy_setopt(h, kOptNoSignal, 1L);   // worker threads
    // the timeouts of the WinHTTP version: 10 s to connect, 30 s without data
    c->easy_setopt(h, kOptConnectTimeout, 10L);
    c->easy_setopt(h, kOptLowSpeedLimit, 1L);
    c->easy_setopt(h, kOptLowSpeedTime, 30L);
    c->easy_setopt(h, kOptErrorBuffer, message);
    c->easy_setopt(h, kOptWriteFunction, (WriteFn)OnData);
    c->easy_setopt(h, kOptWriteData, &t);
    c->easy_setopt(h, kOptXferInfoFunction, (XferFn)OnProgress);
    c->easy_setopt(h, kOptXferInfoData, &t);
    c->easy_setopt(h, kOptNoProgress, 0L);
    CurlCode rc = c->easy_perform(h);
    if (!t.status) c->easy_getinfo(h, kInfoResponseCode, &t.status);
    c->easy_cleanup(h);
    if (progress && progress->cancel)
    {
        error = "canceled";
        return false;
    }
    if (t.tooLarge || rc == kFileSizeExceeded)
    {
        error = std::format("{} is larger than {} MB", url, maxBytes >> 20);
        return false;
    }
    if (t.sinkFailed)
    {
        error = "cannot write the download";
        return false;
    }
    if (t.status && t.status != 200)
    {
        error = std::format("{} answered with status {}", url, t.status);
        return false;
    }
    if (rc != 0)
    {
        std::string why = message[0] ? message : c->easy_strerror(rc);
        if (rc == kWriteError || rc == kAbortedByCallback) why = c->easy_strerror(rc);
        if (t.status) error = std::format("download from {} interrupted ({})", url, why);
        else error = std::format("cannot reach {} ({})", url.substr(0, url.find('/', 8)), why);
        return false;
    }
    return true;
}
