#pragma once
// SHA-1 for checking installed game files (Windows: CNG; Linux: portable code).
#include <cstddef>
#include <cstdint>

class Sha1
{
public:
    Sha1();
    ~Sha1();
    Sha1(const Sha1&) = delete;
    Sha1& operator=(const Sha1&) = delete;
    bool Ok() const { return impl != nullptr; }
    void Update(const void* data, size_t len);
    void Final(uint8_t out[20]);

private:
    void* impl;
};

bool Sha1_Hash(const void* data, size_t len, uint8_t out[20]);
