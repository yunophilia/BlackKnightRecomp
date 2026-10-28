// The mod browser's web images (GameBanana previews: untrusted input) through the system's
// libpng16 and libjpeg-turbo, loaded with dlopen at first use like libcurl: the build needs
// no headers, and without the libraries there are no previews. PNG and JPEG only.
//
// The few libpng and libjpeg names used are declared here. libpng: the simplified API
// (png_image, PNG_IMAGE_VERSION 1). libjpeg: the classic API in its two ABIs, libjpeg.so.8
// (JPEG_LIB_VERSION 80: Debian, Ubuntu, Arch, SteamOS) and libjpeg.so.62 (62: Fedora and
// others). The offsets below are the same in both (checked against both headers); only the
// struct size differs, and jpeg_CreateDecompress refuses a wrong size or version.
#include "../image.h"

#include <dlfcn.h>

#include <csetjmp>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <type_traits>

namespace
{
constexpr uint32_t kMaxSide = 4096;

template <typename Fn>
bool Sym(void* lib, Fn& fn, const char* name)
{
    fn = reinterpret_cast<Fn>(dlsym(lib, name));
    return fn != nullptr;
}

// ---------------------------------------------------------------- PNG (libpng16)
struct PngImage
{
    void* opaque;
    uint32_t version, width, height, format, flags, colormap_entries, warning_or_error;
    char message[64];
};
static_assert(sizeof(PngImage) == 104 && offsetof(PngImage, warning_or_error) == 32);
constexpr uint32_t kPngImageVersion = 1, kPngFormatRgba = 3, kPngImageError = 2;

struct Png
{
    int (*begin)(PngImage*, const void*, size_t);
    int (*finish)(PngImage*, const void*, void*, int32_t, void*);
    void (*free)(PngImage*);
};

const Png* LoadPng()
{
    static Png p;
    static bool ok = false;
    static std::once_flag once;
    std::call_once(once, [] {
        void* lib = dlopen("libpng16.so.16", RTLD_NOW | RTLD_LOCAL);
        ok = lib && Sym(lib, p.begin, "png_image_begin_read_from_memory") && Sym(lib, p.finish, "png_image_finish_read") &&
             Sym(lib, p.free, "png_image_free");
    });
    return ok ? &p : nullptr;
}

bool DecodePng(const uint8_t* data, size_t size, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba)
{
    const Png* p = LoadPng();
    if (!p) return false;
    PngImage img{};
    img.version = kPngImageVersion;
    if (!p->begin(&img, data, size))
    {
        p->free(&img);
        return false;
    }
    // the size from the header, before anything is allocated for the pixels
    if (!img.width || !img.height || img.width > kMaxSide || img.height > kMaxSide)
    {
        p->free(&img);
        return false;
    }
    img.format = kPngFormatRgba;
    rgba.assign((size_t)img.width * img.height * 4, 0);
    bool ok = p->finish(&img, nullptr, rgba.data(), 0, nullptr) && !(img.warning_or_error & kPngImageError);
    p->free(&img);
    if (!ok) return false;
    width = img.width;
    height = img.height;
    return true;
}

// ---------------------------------------------------------------- JPEG (libjpeg-turbo)
// jpeg_decompress_struct fields (both ABIs) and the error manager's function pointers
constexpr size_t kOffErr = 0, kOffMem = 8, kOffImageWidth = 48, kOffImageHeight = 52, kOffOutColorSpace = 64,
                 kOffOutputWidth = 136, kOffOutputHeight = 140, kOffOutputComponents = 148, kOffOutputScanline = 168;
constexpr size_t kErrMgrSize = 168, kOffErrorExit = 0, kOffEmitMessage = 8, kOffOutputMessage = 16;
constexpr size_t kOffMaxMemory = 88;   // in jpeg_memory_mgr
constexpr int kJcsExtRgba = 12;        // libjpeg-turbo's RGBA output
constexpr long kMaxMemory = 256l << 20;

struct Jpeg
{
    int version;
    size_t structSize;
    void* (*std_error)(void*);
    void (*create)(void*, int, size_t);
    void (*mem_src)(void*, const unsigned char*, unsigned long);
    int (*read_header)(void*, int);
    int (*start)(void*);
    uint32_t (*read_scanlines)(void*, unsigned char**, uint32_t);
    void (*destroy)(void*);
};

const Jpeg* LoadJpeg()
{
    static Jpeg j;
    static bool ok = false;
    static std::once_flag once;
    std::call_once(once, [] {
        struct Abi { const char* name; int version; size_t size; };
        for (Abi abi : { Abi{ "libjpeg.so.8", 80, 656 }, Abi{ "libjpeg.so.62", 62, 632 } })
        {
            void* lib = dlopen(abi.name, RTLD_NOW | RTLD_LOCAL);
            if (!lib) continue;
            j.version = abi.version;
            j.structSize = abi.size;
            ok = Sym(lib, j.std_error, "jpeg_std_error") && Sym(lib, j.create, "jpeg_CreateDecompress") &&
                 Sym(lib, j.mem_src, "jpeg_mem_src") && Sym(lib, j.read_header, "jpeg_read_header") &&
                 Sym(lib, j.start, "jpeg_start_decompress") && Sym(lib, j.read_scanlines, "jpeg_read_scanlines") &&
                 Sym(lib, j.destroy, "jpeg_destroy_decompress");
            if (ok) return;
            dlclose(lib);
        }
    });
    return ok ? &j : nullptr;
}

struct JpegError
{
    alignas(8) uint8_t mgr[kErrMgrSize];   // jpeg_error_mgr, first: libjpeg passes it back
    std::jmp_buf jump;
};

template <typename T>
T& Field(void* base, size_t offset)
{
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}

JpegError* ErrorOf(void* cinfo) { return Field<JpegError*>(cinfo, kOffErr); }
void OnError(void* cinfo) { std::longjmp(ErrorOf(cinfo)->jump, 1); }
// warnings (level -1) are corrupt or truncated data: fail instead of showing a partial image
void OnMessage(void* cinfo, int level)
{
    if (level < 0) std::longjmp(ErrorOf(cinfo)->jump, 1);
}
void OnOutput(void*) {}

// The steps that can fail: libjpeg leaves them with longjmp, so they hold no objects with
// destructors.
bool JpegCreate(const Jpeg& j, void* cinfo, JpegError& e)
{
    if (setjmp(e.jump)) return false;
    j.create(cinfo, j.version, j.structSize);
    Field<long>(Field<void*>(cinfo, kOffMem), kOffMaxMemory) = kMaxMemory;
    return true;
}

bool JpegHeader(const Jpeg& j, void* cinfo, JpegError& e, const uint8_t* data, size_t size, uint32_t& w, uint32_t& h)
{
    if (setjmp(e.jump)) return false;
    j.mem_src(cinfo, data, (unsigned long)size);
    if (j.read_header(cinfo, 1) != 1) return false;   // JPEG_HEADER_OK
    w = Field<uint32_t>(cinfo, kOffImageWidth);
    h = Field<uint32_t>(cinfo, kOffImageHeight);
    return true;
}

bool JpegPixels(const Jpeg& j, void* cinfo, JpegError& e, uint8_t* out, uint32_t w, uint32_t h)
{
    if (setjmp(e.jump)) return false;
    Field<int>(cinfo, kOffOutColorSpace) = kJcsExtRgba;
    if (!j.start(cinfo)) return false;
    if (Field<uint32_t>(cinfo, kOffOutputWidth) != w || Field<uint32_t>(cinfo, kOffOutputHeight) != h ||
        Field<int>(cinfo, kOffOutputComponents) != 4)
        return false;
    while (Field<uint32_t>(cinfo, kOffOutputScanline) < h)
    {
        unsigned char* row = out + (size_t)Field<uint32_t>(cinfo, kOffOutputScanline) * w * 4;
        if (j.read_scanlines(cinfo, &row, 1) != 1) return false;
    }
    return true;
}

bool DecodeJpeg(const uint8_t* data, size_t size, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba)
{
    const Jpeg* j = LoadJpeg();
    if (!j) return false;
    alignas(16) uint8_t cinfo[1024] = {};   // jpeg_decompress_struct (632 or 656 bytes)
    JpegError e;
    j->std_error(e.mgr);
    Field<void (*)(void*)>(e.mgr, kOffErrorExit) = OnError;
    Field<void (*)(void*, int)>(e.mgr, kOffEmitMessage) = OnMessage;
    Field<void (*)(void*)>(e.mgr, kOffOutputMessage) = OnOutput;
    Field<JpegError*>(cinfo, kOffErr) = &e;
    uint32_t w = 0, h = 0;
    bool ok = JpegCreate(*j, cinfo, e) && JpegHeader(*j, cinfo, e, data, size, w, h) && w && h && w <= kMaxSide &&
              h <= kMaxSide;
    if (ok)
    {
        rgba.assign((size_t)w * h * 4, 0);
        ok = JpegPixels(*j, cinfo, e, rgba.data(), w, h);
    }
    j->destroy(cinfo);   // also after a failed create: nothing allocated yet (mem is null)
    if (!ok) return false;
    width = w;
    height = h;
    return true;
}
}   // namespace

bool Image_Decode(const void* data, size_t size, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba)
{
    const uint8_t* p = (const uint8_t*)data;
    static const uint8_t kPng[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    bool ok = false;
    if (size >= 8 && !memcmp(p, kPng, 8)) ok = DecodePng(p, size, width, height, rgba);
    else if (size >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) ok = DecodeJpeg(p, size, width, height, rgba);
    if (!ok) rgba.clear();
    return ok;
}
