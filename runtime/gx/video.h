#pragma once
#include <cstdint>
#include <d3d11.h>

struct VideoConfig
{
    int windowWidth = 1280;
    int windowHeight = 720;
    int internalScale = 3;     // EFB resolution multiplier
    bool vsync = false;        // the game paces itself from VI interrupts
    bool widescreen = true;    // present 16:9
    bool fullscreen = false;
};

extern VideoConfig g_videoConfig;
extern ID3D11Device* g_device;
extern ID3D11DeviceContext* g_ctx;

bool Video_Init();
// Presents the given texture region (an XFB copy) to the window.
void Video_Present(ID3D11ShaderResourceView* srv, float u0, float v0, float u1, float v1);
bool Video_WindowClosed();
// Applied on the window thread (settings changed while the game runs)
void Video_SetFullscreen(bool on);
void Video_SetWindowSize(int width, int height);
void* Video_HWND();
