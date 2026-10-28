# Linux

The native Linux build (desktop Linux and the Steam Deck without Proton). Background and
the choices behind it: [linux-native.md](linux-native.md).

## How it is put together

The runtime is the same as on Windows except for `runtime/platform/linux`:

| Area | Windows | Linux |
|---|---|---|
| Window, events | Win32 window thread | SDL3 window on the main thread (the game runs on a second thread with a 256 MiB stack) |
| Graphics | Direct3D 11 | the same D3D11 renderer on [dxvk-native](https://github.com/doitsujin/dxvk) (D3D11 on Vulkan) |
| HLSL compiler | `d3dcompiler_47` | [vkd3d-shader](https://gitlab.winehq.org/wine/vkd3d) (Wine's and Proton's compiler), linked in |
| Input | XInput, `GetAsyncKeyState` | SDL3 gamepads (Steam Input's too), keyboard and mouse; the same bindings and key names |
| Audio | XAudio2 | SDL3 audio streams (PipeWire, PulseAudio or ALSA) |
| Launcher, overlay | ImGui Win32 backend, COM dialogs | ImGui SDL3 backend, SDL file dialog (desktop portal or zenity), SDL message boxes |
| Guest memory | `VirtualAlloc2` placeholders + `MapViewOfFile3` | `memfd_create` + `mmap(MAP_FIXED)` |
| Guest threads | Windows fibers | a small x86-64 context switch |
| Crash reports | vectored exception handler, dbghelp | `SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGFPE` handler with `backtrace()` |
| Writable data | next to the exe | next to the exe for a plain build; `$XDG_DATA_HOME/BlackKnightRecomp` (default `~/.local/share/BlackKnightRecomp`) for the AppImage or a read-only directory |

The Windows-only debugging aids `--watch`, `--profile`, `BKR_SLOW_SAMPLE` and
`BKR_IRQ_STALLS` are not available on Linux.

## Playing

Download `BlackKnightRecomp-<version>-linux-x86_64.AppImage` from the releases, make it
executable (`chmod +x`, or the file's properties in a file manager) and start it. The
launcher asks for your disc image (USA, Europe or Japan; ISO, RVZ, WIA or WBFS) and installs
the game data into `~/.local/share/BlackKnightRecomp/game`. The log, `config.ini`, saves
(`nand/`) and new shaders are kept in the same folder; texture packs go in its `textures/`.

Requirements: x86-64, glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36, current
SteamOS 3 or later), a Vulkan driver (Mesa or NVIDIA), and FUSE 2 to run AppImages (on Ubuntu
`libfuse2t64`; without it, start it with `--appimage-extract-and-run`).

### Steam Deck

1. In Desktop Mode, put the AppImage in your home folder (for example
   `~/Games/BlackKnightRecomp/`) and make it executable (file properties > Permissions).
2. Start it once in Desktop Mode: the launcher asks for your disc image and installs the
   game data.
3. In Steam: *Games > Add a Non-Steam Game to My Library*, choose the AppImage. Do not set
   a Proton version for it (it is a native program).
4. From then on it starts from Game Mode. The Deck's controls act as a controller, so the
   table under Controls in the README applies (View = the button left of the screen,
   Start = the menu button right of it).

## Building

Built and tested on Ubuntu 22.04 (clang 15 with libstdc++ 13 from the
`ppa:ubuntu-toolchain-r/test` PPA, as the release workflow does) and 26.04 (clang 21).
Packages (Debian/Ubuntu names):

```
sudo apt-get install clang ninja-build cmake python3 bison flex libjson-perl libvulkan-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev \
  libxfixes-dev libxinerama-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgl-dev \
  libdrm-dev libgbm-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev \
  libibus-1.0-dev libusb-1.0-0-dev
```

The compiler needs C++20 with `<format>` (clang 15+ with libstdc++ 13+, or libc++ 17+).
The first configure downloads SDL3 3.4.18, dxvk-native 3.1.1, vkd3d 1.19, the Khronos
Vulkan and SPIR-V headers and nod, each pinned by URL and SHA-256 in `CMakeLists.txt`; SDL3
and vkd3d-shader are built from source (SDL3 needs the X11/Wayland/audio headers above,
vkd3d bison, flex, Perl's JSON module and libvulkan). The C++ runtime is linked statically.

The steps are those of the README's Building section with the `linux-clang` preset:

```
cmake --preset linux-clang
cmake --build --preset linux-clang
out/bkr_install path/to/disc.iso game/RENE8P
python3 tools/build_generated.py
cmake --build --preset linux-clang
```

`out/` then has `BlackKnightRecomp` with `libSDL3.so.0` and the two dxvk libraries next to it
(found through `$ORIGIN`). `tools/package_linux.sh` makes the AppImage in `dist/`; with
`-c <dir>` it ships a shader cache made by the Windows package (the release workflow does
this), else it compiles the recorded shaders with vkd3d-shader.

## Testing without a GPU or display

Scripted runs work with Mesa's software Vulkan driver (lavapipe, package
`mesa-vulkan-drivers`) on a virtual X display:

```
Xvfb :97 -screen 0 1280x720x24 &
DISPLAY=:97 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json SDL_AUDIODRIVER=dummy \
  tools/tas_run.sh -s tools/tas/missions/knights_clock.tas -S full
```

`tools/tas_run.sh` is the Linux version of `tools/tas_run.ps1` (docs/tas.md): the same
scripts, test saves (`out/tasnand/`) and outputs (`out/tas/<tag>.log`, `.csv`, `shots/`).
With lavapipe the game runs at full speed through menus and at about 10 fps in 3D stages;
TAS scripts follow guest time, so they reach the same places.

## Checklist for testers

The Linux build was developed and tested with software rendering only (lavapipe in WSL).
What a first test on real hardware should cover, in about 20 minutes:

1. Start the AppImage from a terminal (`./BlackKnightRecomp-*.AppImage`) so its messages
   are visible. The launcher should open; install from your disc image (the file dialog
   should appear; if not, note the desktop environment).
2. Play: the window opens, the intro plays with sound. Check that the log says
   `D3D11 adapter: <your GPU>` and not `llvmpipe`.
3. Controller: menus with the stick and A, View opens the settings overlay, View + Start
   opens the HOME menu, rumble works when you take damage (Settings > Input > Rumble).
4. Keyboard and mouse: WASD/Space/J, the mouse moves the Wii Remote pointer in menus, F1
   opens the settings, F11 toggles fullscreen.
5. Play one mission to its end; note the frame rate (the log prints a `status:` line every
   3 seconds) and any stutter, wrong colours or missing objects.
6. Settings: change the internal resolution (the game restarts by itself) and the window
   size; quit with the overlay's "Quit game" and with the window's close button.
7. Steam Deck only: add the AppImage to Steam (see above), start it from Game Mode, check
   the controls, the sound, sleep/resume, and that the frame rate holds 30 (or 60 with
   the 60 fps setting).

Please report: distribution or SteamOS version, GPU and driver, X11 or Wayland (Desktop or
Game Mode on the Deck), what worked and what didn't, and `BlackKnightRecomp.log`.

## Reporting problems

Attach `BlackKnightRecomp.log` (in `~/.local/share/BlackKnightRecomp/` for the AppImage). It
names the distribution, the SDL video driver (x11 or wayland) and the Vulkan device dxvk
uses. dxvk's own messages go to the terminal; `DXVK_LOG_LEVEL=info` shows more.
