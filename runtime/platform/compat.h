#pragma once
// The MSVC runtime names the shared code uses (byte swaps, *_s functions, _wfopen), for
// other platforms. On Windows this is <intrin.h> and <share.h>; elsewhere it is force-included
// into every runtime file (CMakeLists.txt).
#ifdef _WIN32
#include <intrin.h>
#include <share.h>
#ifndef YieldProcessor
#define YieldProcessor _mm_pause   // as <windows.h> defines it
#endif
#else
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <strings.h>

#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif

inline uint16_t _byteswap_ushort(uint16_t v) { return __builtin_bswap16(v); }
inline uint32_t _byteswap_ulong(uint32_t v) { return __builtin_bswap32(v); }   // 32-bit, as on MSVC
inline uint64_t _byteswap_uint64(uint64_t v) { return __builtin_bswap64(v); }
inline uint32_t _rotl(uint32_t v, int n)
{
    n &= 31;
    return n ? (v << n) | (v >> (32 - n)) : v;
}

#define YieldProcessor() __builtin_ia32_pause()

inline int fopen_s(FILE** f, const char* path, const char* mode)
{
    *f = fopen(path, mode);
    return *f ? 0 : errno;
}

// path.c_str() is char on POSIX; the mode is still written as a wide literal
inline FILE* _wfopen(const char* path, const wchar_t* mode)
{
    char m[8] = {};
    for (size_t i = 0; i < sizeof(m) - 1 && mode[i]; i++) m[i] = (char)mode[i];
    return fopen(path, m);
}

#define _SH_DENYWR 0
inline FILE* _fsopen(const char* path, const char* mode, int) { return fopen(path, mode); }

inline int localtime_s(struct tm* out, const time_t* t) { return localtime_r(t, out) ? 0 : errno; }

#define _stricmp strcasecmp
#define _strnicmp strncasecmp

template <size_t N>
inline int strcpy_s(char (&dst)[N], const char* src)
{
    snprintf(dst, N, "%s", src);
    return 0;
}

inline int strncpy_s(char* dst, size_t size, const char* src, size_t count)
{
    if (!size) return EINVAL;
    size_t n = strnlen(src, count < size ? count : size - 1);
    memcpy(dst, src, n);
    dst[n] = 0;
    return 0;
}

#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _mkgmtime timegm

template <typename T, size_t N>
constexpr size_t _countof(T (&)[N]) { return N; }
#endif
