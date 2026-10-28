#pragma once
// What the launcher, the settings overlay and the installer need from the platform: the
// ImGui platform backend (Win32 or SDL3; rendering is ImGui's D3D11 backend everywhere),
// the launcher's window, and the installer's file and progress dialogs.
#include <cstdint>
#include <filesystem>
#include <string>

// ---------------------------------------------------------------- ImGui platform backend
// For the current ImGui context and a window (HWND / SDL_Window*).
void UiBackend_Init(void* window);
void UiBackend_NewFrame();
void UiBackend_Shutdown();
// Overlay (render thread): feeds ImGui the input the window thread queued for it.
void UiBackend_ReplayInput();

// ---------------------------------------------------------------- launcher window
// Creates the launcher window (hidden until UiWindow_Show), centered on the desktop.
void* UiWindow_Create(const char* title, int width, int height);
void UiWindow_Show();
// Handles pending window events. False once the user closed the window; resizeW/H are the
// new client size when it changed (else 0).
bool UiWindow_Pump(uint32_t& resizeW, uint32_t& resizeH);
void UiWindow_Destroy();

// ---------------------------------------------------------------- the desktop
// Opens a web page in the default browser, or a folder in the file manager.
void Shell_OpenUrl(const std::string& url);
void Shell_OpenFolder(const std::filesystem::path& dir);

// ---------------------------------------------------------------- installer dialogs
// Asks for the disc image to install from (false: canceled).
bool Dialog_PickDiscImage(std::filesystem::path& out);

// Progress of a long operation, with a way to cancel it.
class ProgressDialog
{
public:
    ProgressDialog(const std::string& title, const std::string& line1, const std::string& line2);
    ~ProgressDialog();
    // Shows the progress; true if the user asked to cancel.
    bool Update(uint64_t done, uint64_t total);

private:
    void* impl;
};
