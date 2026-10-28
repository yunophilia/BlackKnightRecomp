#include "../../kernel/memory.h"
#include "../../log.h"

#include <cerrno>
#include <sys/mman.h>
#include <unistd.h>

uint8_t* g_base = nullptr;

// Reserve 4 GiB of address space and map the guest RAM into it from memory files, so
// that the cached/uncached/physical mirrors alias the same pages.
bool Memory_Init()
{
    const uint64_t total = 0x100000000ull;
    void* res = mmap(nullptr, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (res == MAP_FAILED)
    {
        LOG_ERROR("failed to reserve guest address space ({})", strerror(errno));
        return false;
    }
    uint8_t* base = (uint8_t*)res;

    auto makeFile = [](const char* name, size_t size) {
        int fd = memfd_create(name, MFD_CLOEXEC);
        if (fd >= 0 && ftruncate(fd, (off_t)size) != 0)
        {
            close(fd);
            fd = -1;
        }
        return fd;
    };
    int mem1 = makeFile("bkr-mem1", MEM1_SIZE);
    int mem2 = makeFile("bkr-mem2", MEM2_SIZE);
    int lc = makeFile("bkr-lockedcache", 0x10000);
    if (mem1 < 0 || mem2 < 0 || lc < 0)
    {
        LOG_ERROR("cannot create guest memory ({})", strerror(errno));
        return false;
    }
    struct Region { uint32_t addr; uint32_t size; int fd; };
    Region regions[] = {
        { 0x00000000, MEM1_SIZE, mem1 },   // physical (translation off) aliases
        { 0x10000000, MEM2_SIZE, mem2 },
        { 0x80000000, MEM1_SIZE, mem1 },
        { 0x90000000, MEM2_SIZE, mem2 },
        { 0xC0000000, MEM1_SIZE, mem1 },
        { 0xD0000000, MEM2_SIZE, mem2 },
        { LOCKED_CACHE_ADDR, 0x10000, lc },
    };
    for (auto& r : regions)
    {
        void* v = mmap(base + r.addr, r.size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, r.fd, 0);
        if (v != base + r.addr)
        {
            LOG_ERROR("map view failed at {:08x} ({})", r.addr, strerror(errno));
            return false;
        }
    }
    // the mappings keep the memory alive
    close(mem1);
    close(mem2);
    close(lc);
    g_base = base;
    LOG_INFO("guest memory at {}", (void*)base);
    return true;
}
