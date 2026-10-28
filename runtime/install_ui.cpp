// First-run installer: asks for the disc image and installs the game data from it
#include "install/install.h"
#include "install/releases.h"
#include "log.h"
#include "platform/platform.h"
#include "platform/ui.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace
{
constexpr const char* kTitle = "BlackKnightRecomp";

std::string Utf8(const fs::path& p)
{
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

InstallResult RunWithProgress(const fs::path& image, const fs::path& dest, std::string& error)
{
    InstallProgress progress;
    InstallResult result = InstallResult::Canceled;
    ProgressDialog dlg(kTitle, "Installing the game data from your disc image...", Utf8(image));
    std::atomic<bool> finished{ false };
    std::thread worker([&] {
        result = Install_Run(image, dest, progress, error);
        finished = true;
    });
    for (;;)
    {
        for (int i = 0; i < 10 && !finished; i++) Platform_SleepMs(10);
        if (finished) break;
        if (dlg.Update(progress.done, progress.total ? progress.total.load() : 1)) progress.cancel = true;
    }
    worker.join();
    return result;
}
}   // namespace

// Returns true once the data is installed in dest. foundImage: a disc image found next to
// the game, offered first. damage: why the existing install needs replacing (empty when
// nothing is installed yet).
bool InstallUI_Run(const fs::path& dest, const std::string& foundImage, const std::string& damage)
{
    std::string why = damage.empty() ? "The game data is not installed yet."
                                     : "The installed game data is damaged (" + damage + ") and needs reinstalling.";
    bool installed = false;
    fs::path image;
    if (!foundImage.empty() && Release_Find(Install_GameId(foundImage)))
    {
        std::string msg = why + " Install it from\n\n" + foundImage + "?\n\nChoose No to select a different disc image.";
        MsgResult r = Platform_MessageBox(kTitle, msg, MsgButtons::YesNoCancel, MsgIcon::Question);
        if (r == MsgResult::Yes) image = foundImage;
        else if (r == MsgResult::Cancel) return false;
    }
    else
    {
        MsgResult r = Platform_MessageBox(kTitle,
                                          why + "\n\nSelect your own disc image of Sonic and the "
                                          "Black Knight (USA, Europe or Japan) to install it. ISO, RVZ, WIA and WBFS images work. "
                                          "This takes a minute and needs about 3.5 GB; the image is not needed afterwards.",
                                          MsgButtons::OkCancel, MsgIcon::Info);
        if (r != MsgResult::Ok) return false;
    }
    for (;;)
    {
        if (image.empty() && !Dialog_PickDiscImage(image)) break;
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
        std::string msg = "Could not install the game data:\n\n" + error;
        if (r == InstallResult::WriteFailed)
        {
            Platform_MessageBox(kTitle, msg, MsgButtons::Ok, MsgIcon::Error);
            break;
        }
        msg += "\n\nSelect a different disc image?";
        if (Platform_MessageBox(kTitle, msg, MsgButtons::OkCancel, MsgIcon::Error) != MsgResult::Ok) break;
        image.clear();
    }
    return installed;
}
