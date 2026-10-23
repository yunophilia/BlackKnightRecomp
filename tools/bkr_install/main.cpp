// bkr_install: installs the game data from a disc image, the same way the game does on
// first run. Needed before tools/build_generated.py, which reads sys/main.dol.
//
//   bkr_install <disc image> [destination, default game/extracted/DATA]
//   bkr_install --verify [data, default game/extracted/DATA]   full file check
#include "install/install.h"
#include "install/verify.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

// runs work on a thread, calling report every half second until it is done
template <typename Work, typename Report>
static void RunWithReports(Work work, Report report)
{
    std::atomic<bool> done{ false };
    std::thread worker([&] { work(); done = true; });
    while (!done)
    {
        for (int i = 0; i < 50 && !done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!done) report();
    }
    worker.join();
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)   // paths in any language
#else
int main(int argc, char** argv)
#endif
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: bkr_install <disc image (ISO, RVZ, WBFS, ...)> [destination, default game/extracted/DATA]\n");
        return 2;
    }
    if (std::filesystem::path(argv[1]) == "--verify")
    {
        std::filesystem::path data = argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path("game/extracted/DATA");
        VerifyProgress progress;
        std::string problem;
        DataStatus r = DataStatus::Ok;
        RunWithReports([&] { r = Install_FullCheck(data, progress, problem); }, [&] {
            if (progress.total) fprintf(stderr, "\rchecking: %3d%%", (int)(progress.done * 100 / progress.total));
        });
        fprintf(stderr, "\r%-40s\n", r == DataStatus::Ok ? "all game files are intact" : r == DataStatus::Missing ? "not installed" : ("damaged: " + problem).c_str());
        return r == DataStatus::Ok ? 0 : 1;
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
    RunWithReports([&] { result = Install_Run(image, dest, progress, error); }, [&] {
        uint64_t total = progress.total, done = progress.done;
        if (total) fprintf(stderr, "\rinstalling: %3d%% (%llu / %llu MB)", (int)(done * 100 / total),
                           (unsigned long long)(done >> 20), (unsigned long long)(total >> 20));
    });
    if (result != InstallResult::Ok)
    {
        fprintf(stderr, "\ninstall failed: %s\n", error.c_str());
        return 1;
    }
    fprintf(stderr, "\rinstalled to %s%20s\n", std::filesystem::absolute(dest).string().c_str(), "");
    return 0;
}
