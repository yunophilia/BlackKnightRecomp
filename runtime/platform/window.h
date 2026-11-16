#pragma once
// The game window. gx/video.cpp renders into it with Direct3D 11; the platform owns the
// window itself, its events and its thread (Windows: a window thread; Linux: the main
// thread, see Platform_RunGame).

// Creates and shows the window (client area width x height, or fullscreen on the
// desktop's resolution). Called once, from the game thread.
bool Window_Create(int width, int height, bool fullscreen);
// What DXGI takes as the HWND (Windows: the HWND; dxvk-native: the SDL_Window*).
void* Window_Handle();
// Size of the area the swap chain covers, in pixels.
void Window_ClientSize(int& width, int& height);
// The client area changed size since the last call (the swap chain needs resizing).
bool Window_TakeResized();
// Settings changed while the game runs; safe from any thread.
void Window_SetFullscreen(bool on);
void Window_SetSize(int width, int height);
// The window has the keyboard focus (keyboard/mouse input and rumble are only used then).
bool Window_Focused();
// Mouse cursor in some fixed coordinate space (it changes only when the mouse moves).
bool Window_Cursor(int& x, int& y);
// The cursor position (from Window_Cursor) in the client area: 0..1 each; false outside.
bool Window_CursorInClient(int x, int y, float& u, float& v);
