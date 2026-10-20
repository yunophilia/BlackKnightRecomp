#pragma once
#include <d3d11.h>

// Test aid: saves the render target currently bound on ctx as a BMP (UI screenshots for
// automated checks, see BKR_UI_SHOT in launcher.cpp and overlay.cpp).
bool UI_SaveBoundTarget(ID3D11Device* dev, ID3D11DeviceContext* ctx, const char* path);
