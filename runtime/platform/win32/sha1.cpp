#include "../sha1.h"

#include <windows.h>
#include <bcrypt.h>

Sha1::Sha1() : impl(nullptr)
{
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptCreateHash(BCRYPT_SHA1_ALG_HANDLE, &h, nullptr, 0, nullptr, 0, 0) == 0) impl = h;
}

Sha1::~Sha1()
{
    if (impl) BCryptDestroyHash(impl);
}

void Sha1::Update(const void* data, size_t len) { BCryptHashData(impl, (PUCHAR)data, (ULONG)len, 0); }

void Sha1::Final(uint8_t out[20]) { BCryptFinishHash(impl, out, 20, 0); }

bool Sha1_Hash(const void* data, size_t len, uint8_t out[20])
{
    return BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, (PUCHAR)data, (ULONG)len, out, 20) == 0;
}
