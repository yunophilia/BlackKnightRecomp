// PNG through SDL3's built-in loader and writer (web images: image_decode.cpp).
#include "../image.h"

#include <SDL3/SDL.h>

#include <cstring>

void Image_Init() {}

bool Image_LoadPNG(const std::filesystem::path& path, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba)
{
    SDL_Surface* s = SDL_LoadPNG(path.c_str());
    if (!s) return false;
    // ABGR8888 is R, G, B, A in memory
    SDL_Surface* c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(s);
    if (!c) return false;
    width = (uint32_t)c->w;
    height = (uint32_t)c->h;
    rgba.resize((size_t)width * height * 4);
    for (uint32_t y = 0; y < height; y++) memcpy(rgba.data() + (size_t)y * width * 4, (uint8_t*)c->pixels + (size_t)y * c->pitch, width * 4);
    SDL_DestroySurface(c);
    return width && height;
}

bool Image_SavePNG(const std::filesystem::path& path, uint32_t w, uint32_t h, const uint32_t* rgba)
{
    SDL_Surface* s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void*)rgba, (int)w * 4);
    if (!s) return false;
    bool ok = SDL_SavePNG(s, path.c_str());
    SDL_DestroySurface(s);
    return ok;
}
