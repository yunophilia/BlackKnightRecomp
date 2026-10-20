// First-run installer: asks for the disc image and installs the game data from it
#include "install/install.h"
#include "log.h"

#include <filesystem>
#include <string>
#include <thread>
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace fs = std::filesystem;

namespace
{
constexpr const wchar_t* kTitle = L"BlackKnightRecomp";

std::wstring Widen(const std::string& s)
{
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), (int)w.size());
    return w;
}

bool PickImage(fs::path& out)
{
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return false;
    COMDLG_FILTERSPEC filters[] = { { L"Wii disc images", kInstallImageFilter }, { L"All files", L"*.*" } };
    dlg->SetFileTypes(2, filters);
    dlg->SetTitle(L"Select your Sonic and the Black Knight disc image (PAL, RENE8P)");
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

InstallResult RunWithProgress(const fs::path& image, const fs::path& dest, std::string& error)
{
    InstallProgress progress;
    InstallResult result = InstallResult::Canceled;
    IProgressDialog* dlg = nullptr;
    CoCreateInstance(CLSID_ProgressDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (dlg)
    {
        dlg->SetTitle(kTitle);
        dlg->SetLine(1, L"Installing the game data from your disc image...", FALSE, nullptr);
        dlg->SetLine(2, image.c_str(), TRUE, nullptr);
        dlg->StartProgressDialog(nullptr, nullptr, PROGDLG_NORMAL | PROGDLG_AUTOTIME, nullptr);
    }
    std::thread worker([&] { result = Install_Run(image, dest, progress, error); });
    while (WaitForSingleObject(worker.native_handle(), 100) == WAIT_TIMEOUT)
    {
        if (!dlg) continue;
        dlg->SetProgress64(progress.done, progress.total ? progress.total.load() : 1);
        if (dlg->HasUserCancelled()) progress.cancel = true;
    }
    worker.join();
    if (dlg)
    {
        dlg->StopProgressDialog();
        dlg->Release();
    }
    return result;
}
}   // namespace

// Returns true once the data is installed in dest. foundImage: a disc image found next to
// the game, offered first. damage: why the existing install needs replacing (empty when
// nothing is installed yet).
bool InstallUI_Run(const fs::path& dest, const std::string& foundImage, const std::string& damage)
{
    std::wstring why = damage.empty() ? L"The game data is not installed yet."
                                      : L"The installed game data is damaged (" + Widen(damage) + L") and needs reinstalling.";
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool installed = false;
    fs::path image;
    if (!foundImage.empty() && Install_GameId(foundImage) == "RENE8P")
    {
        std::wstring msg = why + L" Install it from\n\n" + Widen(foundImage) +
                           L"?\n\nChoose No to select a different disc image.";
        int r = MessageBoxW(nullptr, msg.c_str(), kTitle, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDYES) image = foundImage;
        else if (r == IDCANCEL) goto done;
    }
    else
    {
        int r = MessageBoxW(nullptr,
                            (why + L"\n\nSelect your own disc image of Sonic and the "
                            L"Black Knight (PAL, RENE8P) to install it. ISO, RVZ, WIA and WBFS images work. "
                            L"This takes a minute and needs about 3.5 GB; the image is not needed afterwards.").c_str(),
                            kTitle, MB_OKCANCEL | MB_ICONINFORMATION);
        if (r != IDOK) goto done;
    }
    for (;;)
    {
        if (image.empty() && !PickImage(image)) break;
        LOG_INFO("installing game data from {} to {}", image.string(), dest.string());
        std::string error;
        InstallResult r = RunWithProgress(image, dest, error);
        if (r == InstallResult::Ok)
        {
            LOG_INFO("game data installed");
            installed = true;
            break;
        }
        if (r == InstallResult::Canceled) break;
        LOG_ERROR("install failed: {}", error);
        std::wstring msg = L"Could not install the game data:\n\n" + Widen(error);
        if (r == InstallResult::WriteFailed)
        {
            MessageBoxW(nullptr, msg.c_str(), kTitle, MB_ICONERROR);
            break;
        }
        msg += L"\n\nSelect a different disc image?";
        if (MessageBoxW(nullptr, msg.c_str(), kTitle, MB_OKCANCEL | MB_ICONERROR) != IDOK) break;
        image.clear();
    }
done:
    if (SUCCEEDED(co)) CoUninitialize();
    return installed;
}
