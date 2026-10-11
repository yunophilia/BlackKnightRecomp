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
* **IOS** (`runtime/ios`): high-level `/dev/di` (served from the extracted disc files, so
  no decryption key is needed), `/dev/fs` (NAND in `nand/` next to the exe; saves land in
  `nand/title/00010000/52454e45/data`), `/dev/es`, `/dev/stm`.
* **Graphics** (`runtime/gx`): the GX command FIFO is decoded and rendered with Direct3D 11.
  Vertex and pixel shaders are generated per GX state (transform/lighting/texgen and the
  integer-exact TEV combiner with indirect texturing, alpha test, fog, z-textures). EFB
  copies become render-to-texture; XFB copies are presented. Renders at 3x internal
  resolution, 16:9.
* **Input** (`runtime/input`): XInput controllers (and keyboard) replace the Wii Remote and
  Nunchuk.

## Building

Requirements: Windows 10+, Visual Studio 2019/2022 with C++, CMake 3.20+, Python 3.10+,
Dolphin (for `DolphinTool.exe` and its signature database).

1. Extract your disc into `game/`:

   ```
   DolphinTool extract -i RENE8P.iso -o game/extracted -g
   ```

   (keep the `.iso` in `game/` as well; it is used for raw disc reads). Copy
   `Dolphin/Sys/GC/dsp_coef.bin` into `game/` too: the audio resampler uses it (without it
   audio falls back to linear resampling).
2. Generate the recompiled sources:

   ```
   python tools/build_generated.py
   ```
3. Build:

   ```
   cmake -S . -B build -G "Visual Studio 16 2019" -A x64 -T host=x64
   cmake --build build --config Release
   ```
   If the link step crashes with `LNK1000` (seen with VS2019 16.10 and the Windows SDK
   10.0.26100 import libraries), use `tools/dev_build.bat`, which links against 10.0.19041.
4. Run `build/Release/BlackKnightRecomp.exe` (it finds `game/` relative to the exe or the
   repo root, or pass `--data <dir> --iso <file>`).

## Controls (default)

| Controller | Keyboard | Action |
|---|---|---|
| Left stick | WASD | Move |
| A | Space | Jump / homing attack |
| X (hold to repeat) | J | Slash (sword swing) |
| Right stick flick | | Directional slash |
| B, then X | K | Soul Surge (Wii Remote B) |
| RB | Left Shift | Guard / talk (Z) |
| LB | Q | Nunchuk C |
| Y | L | Shake Nunchuk |
| RT / LT | U / - | Thrust Remote forward / pull back |
| Start | Enter | Pause (+) |
| Back | Backspace | Minus (-) |
| Back + Start | H | HOME menu |
| Right stick (in menus) | Mouse | Pointer |
| D-pad | Arrow keys | D-pad |

Controllers 2-4 become Wii Remotes 2-4 (for Battle mode) while plugged in; the keyboard
always drives player 1. The Wii Remote pointer follows the mouse when it moves inside the window, otherwise the
right stick moves it. Choosing "Wii Menu" in the HOME menu quits; "Reset" restarts.
F11 or Alt+Enter toggles fullscreen.

## Settings

`config.ini` is created next to the executable on first run:

* `frame_rate`: `30` (original) or `60`. 60 fps works through the game's own frame-rate
  setting, so game speed stays correct; treat it as experimental.
* `internal_scale`: internal resolution multiplier (default 3, about 1080p).
* `window_width`, `window_height`, `fullscreen`, `vsync`, `widescreen`.
* `texture_pack`, `texture_pack_dir`: Dolphin-format HD texture packs (for example the one
  in the GameBanana "Ai HD Texture Overhaul" mod). Put the pack's `RENE8P` folder in
  `textures/` next to the exe or in `game/textures/`; packs installed in Dolphin's
  `Load/Textures` are picked up too. PNG and DDS (BC1/2/3/7) are supported.
  `--dump-textures` writes the game's textures under Dolphin's names to `dump/textures`.
* `language`: english, german, french, spanish, italian, dutch, japanese.
* `rumble`: controller vibration.
* `data`, `iso`: game file locations, if they are not in `game/`.

## Status

The game is playable from boot to gameplay, with graphics, audio, saves and controller
input working. See `docs/progress.md`.

## License

The runtime contains code ported from the Dolphin emulator (GPL-2.0-or-later); the project
as a whole is distributed under the GPL.
