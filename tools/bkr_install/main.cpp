// bkr_install: installs the game data from a disc image, the same way the game does on
// first run. Needed before tools/build_generated.py, which reads sys/main.dol.
//
//   bkr_install <disc image> [destination, default game/extracted/DATA]
#include "install/install.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <windows.h>

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: bkr_install <disc image (ISO, RVZ, WBFS, ...)> [destination, default game/extracted/DATA]\n");
        return 2;
    }
    std::filesystem::path image = argv[1];
    std::filesystem::path dest = argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path("game/extracted/DATA");
    if (std::filesystem::exists(dest))
    {
        fprintf(stderr, "%s already exists; remove it first to reinstall\n", dest.string().c_str());
        return 1;
    }

    InstallProgress progress;
    InstallResult result;
    std::string error;
    std::thread worker([&] { result = Install_Run(image, dest, progress, error); });
    while (WaitForSingleObject(worker.native_handle(), 500) == WAIT_TIMEOUT)
    {
        uint64_t total = progress.total, done = progress.done;
        if (total) fprintf(stderr, "\rinstalling: %3d%% (%llu / %llu MB)", (int)(done * 100 / total),
                           (unsigned long long)(done >> 20), (unsigned long long)(total >> 20));
    }
    worker.join();
    if (result != InstallResult::Ok)
    {
        fprintf(stderr, "\ninstall failed: %s\n", error.c_str());
        return 1;
    }
    fprintf(stderr, "\rinstalled to %s%20s\n", std::filesystem::absolute(dest).string().c_str(), "");
    return 0;
}
