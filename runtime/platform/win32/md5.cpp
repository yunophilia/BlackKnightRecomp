#include "../md5.h"

#include <cstdio>
#include <vector>
#include <windows.h>
#include <bcrypt.h>

bool Md5_File(const std::filesystem::path& file, uint8_t out[16])
{
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) return false;
    BCRYPT_HASH_HANDLE h = nullptr;
    BCryptCreateHash(BCRYPT_MD5_ALG_HANDLE, &h, nullptr, 0, nullptr, 0, 0);
    std::vector<uint8_t> buf(1 << 20);
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) BCryptHashData(h, buf.data(), (ULONG)n, 0);
    fclose(f);
    BCryptFinishHash(h, out, 16, 0);
    BCryptDestroyHash(h);
    return true;
}
