#pragma once
#include <d3d11.h>

// In-game settings overlay (Dear ImGui), opened with the controller's View button or F1.
// The game keeps running underneath; while the overlay is open the game sees no input.
// The window's input reaches it through the platform (platform/*/ui.cpp).
void Overlay_Init(void* window, ID3D11Device* device, ID3D11DeviceContext* ctx);
bool Overlay_Ready();   // initialised (never during TAS runs)
// Render thread, once per presented frame, with the target (w x h) bound.
void Overlay_Render(float w, float h);
// Test aid: a BKR_UI_SHOT_OVERLAY screenshot is due (the frame may be drawn off screen).
bool Overlay_ShotPending();
void Overlay_Toggle();
bool Overlay_IsOpen();
// Asks for a Wii HOME button press (overlay button); read by the gamepad code.
bool Overlay_TakeHomePress();
