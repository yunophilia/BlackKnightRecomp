# BlackKnightRecomp

A static recompilation of **Sonic and the Black Knight** (Wii, PAL `RENE8P`) to native
Windows x64, in the spirit of [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp).
The goal: play it natively on PC with a normal controller instead of Wii Remote motion
controls.

**You must own the game.** Nothing from the game is in this repository; the build reads your
own disc image.

## How it works

```
main.dol ──► tools/analyze.py ──► tools/recomp.py ──► generated/*.cpp ─┐
 (PowerPC)    function discovery    PPC → C++ codegen   (28k functions)  ├─► BlackKnightRecomp.exe
                                                        runtime/ ────────┘
```

* **Recompiler** (`tools/`): decodes every Gekko/Broadway instruction (incl. paired singles
  and quantized loads/stores) and emits one C++ function per guest function. Guest RAM is a
  4 GiB host reservation with MEM1/MEM2 mirrored at their cached/uncached addresses, so
  guest pointers map directly to host memory. Function names come from the disc's
  `BLACKKNIGHT.sel` symbol table and Dolphin's signature database.
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
  `nand/title/00010000/52454e45/data`), `/dev/es`, `/dev/stm`.
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

Run `BlackKnightRecomp.exe`. On first run it asks for your own disc image of the game (PAL,
`RENE8P`; ISO, RVZ, WIA or WBFS) and installs the game data from it into `game/extracted/`
next to the exe, which takes about a minute and 3.5 GB. After that the image is no longer
needed. Mods can replace files under `game/extracted/DATA/files/`.

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
2. Install the game data from your disc image into `game/extracted/DATA`:

   ```
   out/bkr_install path/to/your/disc.iso
   ```
3. Generate the recompiled sources:

   ```
   python tools/build_generated.py
   ```
   The analysis names the SDK functions (for `tools/xref.py` and reports) from
   `tools/sdk_names.txt`, recorded once from Dolphin's signature database; pass
   `--dsy <totaldb.dsy>` to rematch. The generated code is the same either way.
   Optional: the HOME menu module (`hbm_part.rso`) runs in the built-in interpreter
   unless it is recompiled too. Run the game once with `BKR_DUMPMEM2=1` set (it writes
   `mem2_dump.bin` next to the exe when the module is first called; move it to `out/`),
   then `python tools/rso.py` and rerun `python tools/build_generated.py`. The runtime
   only uses the recompiled module if the code in memory matches it exactly.
4. Build the game (CMake reconfigures by itself now that `generated/` exists):

   ```
   cmake --build --preset windows-clang
   ```
   The exe is written to `out/`. Rebuilds are incremental, so after editing the runtime
   only the build command is needed (`tools/dev_build.bat` runs it). Other presets:
   `windows-msvc` (run it from a Visual Studio x64 Developer prompt; with VS2019 use `vcvars64.bat 10.0.19041.0`, its linker crashes on newer SDKs) and `linux-clang`,
   which only builds the recompiled code until the runtime is ported off D3D11/XAudio2.
   `tools/package.ps1` collects the exe and README into `dist/BlackKnightRecomp/`.
5. Run `out/BlackKnightRecomp.exe` (it finds `game/` relative to the exe or the
   repo root, or pass `--data <dir>`). Data extracted earlier with DolphinTool
   (`DolphinTool extract -i RENE8P.iso -o game/extracted -g`) still works; keep the
   `.iso` in `game/` in that case, it is used for raw disc reads.

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

Controller, keyboard and mouse bindings can be changed in the settings (launcher or in
game) or in `config.ini` (`[Input]` and `[Keyboard]`; a keyboard action can have several
keys or mouse buttons, e.g. `slash = J, MOUSE1`). The table shows the defaults.
Controllers 2-4 become Wii Remotes 2-4 (for Battle mode) while plugged in; the keyboard
always drives player 1. The Wii Remote pointer follows the mouse when it moves inside the window, otherwise the
right stick moves it. Choosing "Wii Menu" in the HOME menu quits; "Reset" restarts.
F11 or Alt+Enter toggles fullscreen.

## Launcher and settings

The game opens a launcher on every start: Play, settings, and the game data status. It
asks for your disc image only when nothing is installed yet or the installed files are
damaged (each start checks that every file is present with the right size; "Check all
game files" compares the hash of every file). `bkr_install --verify` does the full check
from the command line.

In game, View (Back) or F1 opens the same settings over the running game. Changes are
saved to `config.ini` at once and most apply immediately; frame rate, internal
resolution, widescreen, language, texture packs and the graphics thread ask to restart
the game. The overlay also has the Wii HOME Menu, restart and quit.

`config.ini` next to the executable holds the settings (it can also be edited by hand):

* `frame_rate`: `30` (original) or `60`. Game logic runs at a fixed 60 Hz either way, so
  game time and speed match the original in every stage type (see docs/progress.md).
* `internal_scale`: internal resolution multiplier (default 3, about 1080p).
* `window_width`, `window_height`, `fullscreen`, `vsync`, `widescreen`.
* `texture_pack`, `texture_pack_dir`: Dolphin-format HD texture packs (for example the one
  in the GameBanana "Ai HD Texture Overhaul" mod). Put the pack's `RENE8P` folder in
  `textures/` next to the exe or in `game/textures/`; packs installed in Dolphin's
  `Load/Textures` are picked up too. PNG and DDS (BC1/2/3/7) are supported.
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

## Testing

* `tools/smoke_test.ps1 [-Fps60]`: boots into mission 1 with a scripted input run and
  checks the log (no errors, expected milestones, frame rate) and screenshots.
* `tools/gen_soak.py`: random-input scripts for long stability runs (see its header).
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

No game code or data is included; you need your own copy of the game.
