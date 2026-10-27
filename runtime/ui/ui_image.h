#pragma once
#include <imgui.h>

#include <string>

struct ID3D11Device;

// Web images for the mod browser (GameBanana thumbnails and screenshots): downloaded and
// decoded on a worker thread, turned into textures of the current UI's D3D11 device.

// The device of the window the settings are drawn in (launcher or game); nullptr releases
// every texture (before that device goes away).
void UiImage_SetDevice(ID3D11Device* device);
// The image's texture once it is loaded (starts loading it), else a null id. size gets its
// pixel size.
ImTextureID UiImage_Get(const std::string& url, ImVec2* size = nullptr);
