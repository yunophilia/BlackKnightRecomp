# BlackKnightRecomp

A static recompilation of **Sonic and the Black Knight** (Wii) to native Windows x64, in the
spirit of [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp).
The goal: play it natively on PC with a normal controller instead of Wii Remote motion
controls.

Supported discs: USA (`RENE8P`), Europe (`RENP8P`) and Japan (`RENJ8P`). One exe plays all
three: it contains the recompiled code of each, and the installed disc picks which runs.

**You must own the game.** Nothing from the game is in this repository; the build reads your
own disc image.

## How it works

```
main.dol ──► tools/analyze.py ──► tools/recomp.py ──► generated/<release>/*.cpp ─┐
 (PowerPC)    function discovery    PPC → C++ codegen   (28k functions each)       ├─► BlackKnightRecomp.exe
 per release                                            runtime/ ──────────────────┘
```

* **Recompiler** (`tools/`): decodes every Gekko/Broadway instruction (incl. paired singles
  and quantized loads/stores) and emits one C++ function per guest function. Guest RAM is a
  4 GiB host reservation with MEM1/MEM2 mirrored at their cached/uncached addresses, so
  guest pointers map directly to host memory. Function names come from the disc's
  `BLACKKNIGHT.sel` symbol table and Dolphin's signature database. Each release's code is
  in its own namespace with its own dispatch table. The releases differ only slightly (the
  code is a few hundred bytes longer or shorter, so most addresses move), so the runtime
  refers to game functions and variables by name: `tools/game_addrs.txt` lists them for
  the USA release, and `tools/game_addrs.py` finds them in the others by matching code
  (`runtime/game_addrs.inc`).
* **CPU runtime** (`runtime/cpu`): SPRs, time base / decrementer, exact `fres`/`frsqrte`
  tables, indirect call dispatch.
* **OS** (`runtime/kernel`): the game's *own* Revolution OS (scheduler, alarms, interrupt
  dispatch) runs recompiled. Only the CPU-level primitives are emulated: every guest thread
  is a host fiber (`OSSaveContext`/`OSLoadContext` become fiber switches) and external /
  decrementer exceptions are delivered at safe points.
* **Hardware** (`runtime/hw`): memory-mapped VI, PI, CP/PE, DSP/AI, EXI (RTC/SRAM), SI and
  the Hollywood IPC registers, modelled on Dolphin's behaviour.
* **IOS** (`runtime/ios`): high-level `/dev/di` (served from the installed disc files, which
  the installer decrypts once with nod), `/dev/fs` (NAND in `nand/` next to the exe; saves land in
  `nand/title/00010000/<id>/data`: `52454e45` USA, `52454e50` Europe, `52454e4a` Japan),
  `/dev/es`, `/dev/stm`.
* **Graphics** (`runtime/gx`): the GX command FIFO is decoded and rendered with Direct3D 11
  on a separate thread (the CPU thread only fills a ring buffer, like Dolphin's dual-core
  mode).
  Vertex and pixel shaders are generated per GX state (transform/lighting/texgen and the
  integer-exact TEV combiner with indirect texturing, alpha test, fog, z-textures). EFB
  copies become render-to-texture; XFB copies are presented; CPU EFB peeks are served from
  a read-back snapshot. Renders at 3x internal resolution, 16:9. Compiled shaders are
  cached in `shadercache/`, and Dolphin-format HD texture packs are supported.
* **Input** (`runtime/input`): the WPAD library is replaced at the API level; KPAD runs
  recompiled on top of it. XInput controllers (and the keyboard) become Wii Remotes with a
  Nunchuk: sword swings are synthesised as accelerometer pulses, the IR pointer comes
  from the mouse or right stick, and the remote speaker stream is decoded and played.
* **Audio** (`runtime/audio`): the AX DSP microcode is replaced by a port of Dolphin's AX
  HLE; output goes through XAudio2.

## Playing

Run `BlackKnightRecomp.exe`. On first run it asks for your own disc image of the game (USA
`RENE8P`, Europe `RENP8P` or Japan `RENJ8P`; ISO, RVZ, WIA or WBFS): select it in the file
dialog, wherever it is on your PC.
There is no need to copy it anywhere. The launcher installs the game data from it into
`game/extracted/` next to the exe, which takes about a minute and 3.5 GB. After that the
image is no longer needed. Mods can replace files under `game/extracted/DATA/files/`. To
switch to another release, choose "Reinstall from disc image..." in the launcher and select
that disc; each release keeps its own saves.

### Steam Deck and Linux (Proton)

There is no native Linux build (see [docs/linux-native.md](docs/linux-native.md)); the
Windows build runs through Proton. v1.0.0 showed a black screen under Proton while the audio
played (issue #5): Wine's HLSL compiler rejected the game's vertex shaders. Later builds
avoid that, and releases include precompiled shaders in `shadercache/`, so keep that folder
next to the exe.

Tested so far: Valve's Proton Experimental, Proton 10.0 and Proton 9.0, and Wine 10.0 with
DXVK 2.7.1, on desktop Linux with software Vulkan, from boot into gameplay, also with no
`shadercache/` at all (every shader compiled by Proton's own compiler). v1.0.1 showed wrong
colours under Proton 10.0 for shaders it compiled itself; that is fixed. **Not yet tested on
a Steam Deck**; reports are welcome.
Setup:

1. In Desktop Mode, unzip the release somewhere in your home folder (for example
   `~/Games/BlackKnightRecomp`), and put your disc image there in `game/` (an `.iso`
   there is offered automatically; RVZ, WIA or WBFS can be picked in the file dialog).
2. In Steam: *Games > Add a Non-Steam Game to My Library*, browse to
   `BlackKnightRecomp.exe` (set the file type filter to "All files").
3. In the shortcut's *Properties > Compatibility*, tick *Force the use of a specific Steam
   Play compatibility tool* and pick Proton Experimental, 10.0 or 9.0.
4. Start it once in Desktop Mode: the launcher installs the game data from your image.
   After that it can be started from Game Mode.

Controls (Steam's defaults for non-Steam games, not checked on a Deck yet): the Deck acts
as an Xbox controller, so the table under [Controls](#controls-default) applies (View = the button left
of the screen, Start = the menu button right of it). In the launcher, the D-pad/left stick
and A work; the touchscreen or the right trackpad act as a mouse.

If it still shows a black screen, `BlackKnightRecomp.log` next to the exe says why (it
logs "running under Wine", the HLSL compiler in use, and any shader or D3D error); please
attach it to an issue.

## Building

Requirements:

- Windows 10+
- Visual Studio 2026 (2022 or newer; the free Build Tools are enough) with the "Desktop
  development with C++" workload and the "C++ Clang Compiler for Windows" component
- CMake 3.21+ and Ninja (both included with Visual Studio)
- Python 3.10+
- An internet connection for the first configure: it downloads the prebuilt
  [nod](https://github.com/encounter/nod) library that reads disc images
- Optional: a newer standalone [LLVM](https://github.com/llvm/llvm-project/releases)
  (`winget install LLVM.LLVM`); the presets use whichever `clang-cl` comes first on `PATH`

Run the commands from a "Developer PowerShell for VS", or use `tools/dev_build.bat` for the
build steps, which sets that up.

1. Configure and build the installer (before the game code is generated, only
   `bkr_install` is built):

   ```
   cmake --preset windows-clang
   cmake --build --preset windows-clang
   ```
2. Install the game data of each release you have into `game/<game ID>`:

   ```
   out/bkr_install path/to/your/usa-disc.iso game/RENE8P
   out/bkr_install path/to/your/europe-disc.rvz game/RENP8P
   out/bkr_install path/to/your/japan-disc.rvz game/RENJ8P
   ```
   One is enough: the exe then contains only that release's code. `game/extracted/DATA`
   (the default destination, where the game itself installs) also counts for the release
   installed there.
3. Generate the recompiled sources:

   ```
   python tools/build_generated.py
   ```
   It generates the code of every release whose data is there, into `generated/<release>/`;
   per-release intermediate files go to `out/<game ID>/`. With all three releases it also
   refreshes `runtime/game_addrs.inc`, the game addresses the runtime uses in each release.
   The analysis names the SDK functions (for `tools/xref.py` and reports) from
   `tools/sdk_names.txt`, recorded once from Dolphin's signature database; pass
   `--dsy <totaldb.dsy>` to rematch. The generated code is the same either way.
   Optional: the HOME menu module (`hbm_part.rso`) runs in the built-in interpreter
   unless it is recompiled too. For each release, run the game once with `BKR_DUMPMEM2=1`
   set and open the HOME menu (it writes `mem2_dump.bin` next to the exe when the module is
   first called; move it to `out/<game ID>/`), then rerun `python tools/build_generated.py`.
   The runtime only uses the recompiled module if the code in memory matches it exactly.
4. Build the game (CMake reconfigures by itself now that `generated/` exists):

   ```
   cmake --build --preset windows-clang
   ```
   The exe is written to `out/`. Rebuilds are incremental, so after editing the runtime
   only the build command is needed (`tools/dev_build.bat` runs it). Other presets:
   `windows-msvc` (run it from a Visual Studio x64 Developer prompt; with VS2019 use `vcvars64.bat 10.0.19041.0`, its linker crashes on newer SDKs) and `linux-clang`,
   which only builds the recompiled code until the runtime is ported off D3D11/XAudio2.
   `tools/package.ps1` collects the exe and README into `dist/BlackKnightRecomp/`, with a
   shader cache precompiled from the GX states recorded in `data/shadercache/`
   (`BlackKnightRecomp.exe --precompile-shaders <dir>` compiles every recorded key that has
   no blob yet).
5. Run `out/BlackKnightRecomp.exe` (it finds `game/extracted/DATA` relative to the exe or
   the repo root, or pass `--data <dir>`, e.g. `--data game/RENP8P`). Data extracted earlier
   with DolphinTool (`DolphinTool extract -i RENE8P.iso -o game/extracted -g`) still works;
   keep the `.iso` in `game/` in that case, it is used for raw disc reads.

## Controls (default)

| Controller | Keyboard | Action |
|---|---|---|
| Left stick | WASD | Move |
| A | Space | Jump / homing attack |
| X (hold to repeat) | J / left click | Slash (sword swing) |
| Right stick flick | | Directional slash |
| B, then X | K | Soul Surge (Wii Remote B) |
| RB | Left Shift / right click | Guard / talk (Z) |
| LB | Q | Nunchuk C |
| Y | L | Shake Nunchuk |
| RT / LT | U / - | Thrust Remote forward / pull back |
| Start | Enter | Pause (+) |
| View (Back) | F1 | Settings overlay (the game keeps running) |
| | Backspace | Wii - (unused by the game; unbound on the controller by default) |
| View + Start | H | HOME menu |
| Right stick (in menus) | Mouse | Pointer |
| D-pad | Arrow keys | D-pad |
| Left stick click | | Wii Remote 1 |
| Right stick click | | Wii Remote 2 (e.g. deleting a save file) |

Controller, keyboard and mouse bindings can be changed in the settings (launcher or in
game) or in `config.ini` (`[Input]` and `[Keyboard]`; a keyboard action can have several
keys or mouse buttons, e.g. `slash = J, MOUSE1`). The table shows the defaults.
Controllers 2-4 become Wii Remotes 2-4 (for Battle mode) while plugged in; the keyboard
always drives player 1. The Wii Remote pointer follows the mouse when it moves inside the window, otherwise the
right stick moves it. Choosing "Wii Menu" in the HOME menu quits; "Reset" restarts.
F11 or Alt+Enter toggles fullscreen.

## Launcher and settings

The game opens a launcher on every start: Play (with the game data status), Settings and Mods. It
asks for your disc image only when nothing is installed yet or the installed files are
damaged (each start checks that every file is present with the right size; "Check all
game files" compares the hash of every file). `bkr_install --verify` does the full check
from the command line.

In game, View (Back) or F1 opens the same settings over the running game. Changes are
saved to `config.ini` at once and most apply immediately; frame rate, internal
resolution, widescreen, language, texture packs, mods and the graphics thread ask to restart
the game. The overlay also has the Wii HOME Menu, restart and quit.

With a controller, the D-pad moves (moving onto a tab opens it), A selects and B goes back;
LB/RB switch between the main tabs and LT/RT between the tabs inside a page (Video, Input, ... or Installed / Get mods).

`config.ini` next to the executable holds the settings (it can also be edited by hand):

* `frame_rate`: `30` (original) or `60`. Game logic runs at a fixed 60 Hz either way, so
  game time and speed match the original in every stage type (see docs/progress.md).
* `internal_scale`: internal resolution multiplier (default 3, about 1080p).
* `window_width`, `window_height`, `fullscreen`, `vsync`, `widescreen`.
* `texture_pack`, `texture_pack_dir`: Dolphin-format HD texture packs (for example the one
  in the GameBanana "Ai HD Texture Overhaul" mod). Put the pack's folder (named by the game ID,
  e.g. `RENE8P`) in `textures/` next to the exe or in `game/textures/`; packs installed in
  Dolphin's `Load/Textures` are picked up too. A pack made for another release is used when
  there is none for the installed one. PNG and DDS (BC1/2/3/7) are supported.
  `--dump-textures` writes the game's textures under Dolphin's names to `dump/textures`.
* `anisotropy`: force anisotropic filtering (1, 2, 4, 8 or 16).
* `gpu_thread`: run GX command processing and rendering on a second core (default on;
  `--single-core` turns it off for debugging).
* `language`: english, german, french, spanish, italian, dutch, japanese.
* `pause_on_focus_loss`: freeze the game while its window is in the background.
* `rumble`: controller vibration.
* `remote_speaker`: play the sounds the game sends to the Wii Remote speaker (decoded
  from the remote's 4-bit ADPCM stream) through the PC's audio output.
* `jump`, `slash`, `wii_b`, `guard`, ...: controller button bindings (`A`, `B`, `X`, `Y`,
  `LB`, `RB`, `LT`, `RT`, `START`, `BACK`, `LS`, `RS`, D-pad `UP`/`DOWN`/`LEFT`/`RIGHT`,
  or `NONE`).
* `data`, `iso`: game file locations, if they are not in `game/`.
* `[Mods]`, `mods`: mod load order and on/off state, and the mods folder if it is not
  `mods/` (see below).

## Mods

Besides texture packs (above), most mods for the game replace game files: character
skins, HUD changes and the like.

**Getting mods in the game**: the Mods tab > "Get mods (GameBanana)" (in the launcher
and in game) lists the game's mods on [GameBanana](https://gamebanana.com/games/6511) with
screenshots and descriptions. Install downloads one and puts it where the game loads it:
game files into `mods/`, Dolphin-format textures into `textures/<game ID>/`. Mods that change
the game's code (a `main.dol`, code patches) cannot work with the recompiled game and are
refused with an explanation; Dolphin settings and Gecko codes in an archive are skipped.
Archives can be zip, 7z or RAR (up to 4 GB unpacked and 100,000 files).
Installed mods show when an update is out, and Remove moves a mod to the Recycle Bin.
`mods/gamebanana.json` remembers what was installed from there. The list is only loaded when
you open that tab. It works with a controller and the keyboard too: on a list entry, A (keyboard: Space)
goes to its buttons and B back.

**Adding mods by hand**: put each mod's folder in `mods/` next to the
executable (`game/mods/` works too). The files in a mod folder mirror the disc's `files/`
folder (`sonic.one`, `adx/`, `event/`, `sound/`, ...), either directly in the mod folder or
in a `files/` subfolder. An optional `mod.ini` gives the mod's `name` (or `title`),
`author`, `version` and `description`; without one the folder name is shown.

Mods made for the Storybook Mod Manager (most of the game's mods on GameBanana) work as
they are: extract the archive and drop the mod's folder (the one with `mod.ini`) into
`mods/`. Their optional extras (the manager's "Configure" choices, usually in an
`Additional Files` folder) are not applied; copy such a file over the one at the mod's top
level to use it instead.

* The installed game files are never changed: when the game reads a file, the first enabled
  mod in the load order that has it supplies it, otherwise the installed file is read. A mod
  file may be larger or smaller than the original. Mod files that are not game files are
  ignored.
* New mods are enabled and load after the ones already known (an archive with several
  variants installs each as its own mod, the first enabled). The Mods tab
  (launcher and in game) lists them with author and version, turns them on or off and changes
  the load order; changes in game ask to restart. The launcher shows which mods are active.
* The file checks (on start and "Check all game files") look at the installed files only,
  so mods never make the game data count as damaged.
* Mods apply to every release (USA, Europe, Japan): most replace files that have the same
  name in all of them. A mod for one release only can say so in its `mod.ini` with
  `releases = RENE8P` (game IDs or `USA`, `Europe`, `Japan`, comma-separated).
* `BlackKnightRecomp.log` lists the files each mod replaces and warns when two enabled mods
  replace the same file (the one higher in the load order wins).

## Testing

* `tools/smoke_test.ps1 [-Fps60]`: boots into mission 1 with a scripted input run and
  checks the log (no errors, expected milestones, frame rate) and screenshots.
* `tools/gen_soak.py`: random-input scripts for long stability runs (see its header).
* `out/bkr_mods_test.exe`: checks the mods code without the game or the network: the file
  overlay (load order, size changes, partial reads) on a small synthetic disc, installing
  zip/7z/RAR archives (layouts, texture packs, refused code mods, unsafe and long paths,
  device names, other formats, size limits, updates, removal, edited or injected records,
  the move from gamebanana.ini) and reading GameBanana's answers. `bkr_mods_test --install <archive> --data
  <game data> --mods <dir> --textures <dir>` installs a real archive (`gamebanana:<id>`:
  downloads that mod) with the same code. `--mods <folder>` makes the game load mods from
  another folder for one run.
* `--capture <frame>` writes a per-draw log, textures and EFB states for one frame;
  `--profile <start> <seconds>` (plus `--profile-gx`) prints a sampling profile.

## Status

The game is playable from boot to gameplay, with graphics, audio, saves and controller
input working. See `docs/progress.md`.

## License

BlackKnightRecomp is licensed under the GNU General Public License, version 2 only
(GPL-2.0-only); see [LICENSE](LICENSE).

Parts of the runtime are ported from or closely follow the
[Dolphin emulator](https://github.com/dolphin-emu/dolphin) (GPL-2.0-or-later), notably the
AX audio HLE, the TEV/lighting/fog shader formulas, VI timing and the DSP/IOS behaviour.
Thanks to the Dolphin team.

The exe also contains [Dear ImGui](https://github.com/ocornut/imgui) (MIT),
[nod](https://github.com/encounter/nod) (MIT), [libarchive](https://libarchive.org) (BSD-2-Clause),
[zlib](https://zlib.net) (zlib license), liblzma from [XZ Utils](https://tukaani.org/xz/) (0BSD)
and [nlohmann/json](https://github.com/nlohmann/json) (MIT); their licences are in `licenses/`
next to the exe.

No game code or data is included; you need your own copy of the game.
