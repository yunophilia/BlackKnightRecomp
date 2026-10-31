// Launcher window, overlay input and installer dialogs on Windows (Win32, COM dialogs and
// ImGui's Win32 backend).
#include "../ui.h"
#include "../../install/install.h"
#include "../../ui/overlay.h"

#include <algorithm>
#include <imgui.h>
#include <imgui_impl_win32.h>

#include <mutex>
#include <vector>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static std::wstring Widen(const std::string& s)
{
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), (int)w.size());
    return w;
}

// ---------------------------------------------------------------- ImGui platform backend
void UiBackend_Init(void* window) { ImGui_ImplWin32_Init((HWND)window); }
void UiBackend_NewFrame() { ImGui_ImplWin32_NewFrame(); }
void UiBackend_Shutdown() { ImGui_ImplWin32_Shutdown(); }

// The overlay's input: window messages, replayed on the render thread
namespace
{
std::mutex s_msgMutex;
std::vector<MSG> s_msgs;

bool IsInput(UINT msg)
{
    return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
           msg == WM_INPUTLANGCHANGE || msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_MOUSELEAVE;
}
}   // namespace

// Window thread (platform/win32/window.cpp): forwards input to the overlay. True if the
// message was consumed.
bool Overlay_WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!Overlay_Ready() || !IsInput(msg)) return false;
    if (msg == WM_KEYDOWN && (wp == VK_F1 || (wp == VK_ESCAPE && Overlay_IsOpen())))
    {
        Overlay_Toggle();
        return true;
    }
    if (!Overlay_IsOpen()) return false;
    std::lock_guard lk(s_msgMutex);
    MSG m{};
    m.hwnd = hwnd;
    m.message = msg;
    m.wParam = wp;
    m.lParam = lp;
    s_msgs.push_back(m);
    return msg != WM_SETFOCUS && msg != WM_KILLFOCUS;   // the game sees no keys or clicks while open
}

void UiBackend_ReplayInput()
{
    std::vector<MSG> msgs;
    {
        std::lock_guard lk(s_msgMutex);
        msgs.swap(s_msgs);
    }
    for (auto& m : msgs) ImGui_ImplWin32_WndProcHandler(m.hwnd, m.message, m.wParam, m.lParam);
}

// ---------------------------------------------------------------- launcher window
namespace
{
HWND s_launcher = nullptr;
HRESULT s_co = E_FAIL;
bool s_quit = false;
UINT s_resizeW = 0, s_resizeH = 0;
const wchar_t* const kLauncherClass = L"BlackKnightRecompLauncher";

LRESULT CALLBACK LauncherProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return TRUE;
    switch (msg)
    {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) { s_resizeW = LOWORD(lp); s_resizeH = HIWORD(lp); }
        return 0;
    case WM_CLOSE:
        s_quit = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}   // namespace

void* UiWindow_Create(const char* title, int w, int h)
{
    s_co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // the file dialog
    WNDCLASSW wc{};
    wc.lpfnWndProc = LauncherProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kLauncherClass;
    RegisterClassW(&wc);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    w = std::min(w, (int)(wa.right - wa.left) * 9 / 10);
    h = std::min(h, (int)(wa.bottom - wa.top) * 9 / 10);
    s_launcher = CreateWindowW(wc.lpszClassName, Widen(title).c_str(), WS_OVERLAPPEDWINDOW, wa.left + (wa.right - wa.left - w) / 2,
                               wa.top + (wa.bottom - wa.top - h) / 2, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    s_quit = false;
    s_resizeW = s_resizeH = 0;
    return s_launcher;
}

void UiWindow_Show()
{
    ShowWindow(s_launcher, SW_SHOWDEFAULT);
    UpdateWindow(s_launcher);
}

bool UiWindow_Pump(uint32_t& resizeW, uint32_t& resizeH)
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    resizeW = s_resizeW;
    resizeH = s_resizeH;
    s_resizeW = s_resizeH = 0;
    return !s_quit;
}

void UiWindow_Destroy()
{
    DestroyWindow(s_launcher);
    UnregisterClassW(kLauncherClass, GetModuleHandleW(nullptr));
    s_launcher = nullptr;
    if (SUCCEEDED(s_co)) CoUninitialize();
    s_co = E_FAIL;
}

// ---------------------------------------------------------------- the desktop
void Shell_OpenUrl(const std::string& url) { ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

void Shell_OpenFolder(const std::filesystem::path& dir)
{
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------- installer dialogs
// COM for the shell dialogs, also when no launcher window has initialised it
struct ComScope
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

bool Dialog_PickDiscImage(std::filesystem::path& out)
{
    ComScope com;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return false;
    COMDLG_FILTERSPEC filters[] = { { L"Wii disc images", kInstallImageFilter }, { L"All files", L"*.*" } };
    dlg->SetFileTypes(2, filters);
    dlg->SetTitle(L"Select your Sonic and the Black Knight disc image (USA, Europe or Japan)");
    bool ok = false;
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->Show(nullptr)) && SUCCEEDED(dlg->GetResult(&item)))
    {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        {
            out = path;
            ok = true;
            CoTaskMemFree(path);
        }
        item->Release();
    }
    dlg->Release();
    return ok;
}

struct ProgressImpl
{
    ComScope com;
    IProgressDialog* dlg = nullptr;
};

ProgressDialog::ProgressDialog(const std::string& title, const std::string& line1, const std::string& line2) : impl(new ProgressImpl())
{
    IProgressDialog*& dlg = ((ProgressImpl*)impl)->dlg;
    CoCreateInstance(CLSID_ProgressDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (dlg)
    {
        dlg->SetTitle(Widen(title).c_str());
        dlg->SetLine(1, Widen(line1).c_str(), FALSE, nullptr);
        dlg->SetLine(2, Widen(line2).c_str(), TRUE, nullptr);
        dlg->StartProgressDialog(nullptr, nullptr, PROGDLG_NORMAL | PROGDLG_AUTOTIME, nullptr);
    }
}

ProgressDialog::~ProgressDialog()
{
    ProgressImpl* p = (ProgressImpl*)impl;
    if (p->dlg)
    {
        p->dlg->StopProgressDialog();
        p->dlg->Release();
    }
    delete p;
}

bool ProgressDialog::Update(uint64_t done, uint64_t total)
{
    IProgressDialog* dlg = ((ProgressImpl*)impl)->dlg;
    if (!dlg) return false;
    dlg->SetProgress64(done, total);
    return dlg->HasUserCancelled();
}
