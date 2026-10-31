#pragma once
// Shared by the Linux platform files (SDL3).
#include <SDL3/SDL.h>

// Main thread, for every event: controllers being added and removed (input.cpp).
void Input_HandleEvent(const SDL_Event& e);
// Main thread: the settings overlay takes the input while it is open (overlay_backend.cpp).
bool Overlay_HandleEvent(const SDL_Event& e);
