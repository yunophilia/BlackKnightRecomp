# Progress log

## Milestones

- [x] Disc extraction, DOL parsing, SEL symbol table (280 named SDK/CRI functions)
- [x] PPC (Gekko/Broadway) decoder covering every opcode in `main.dol`
- [x] Function analysis: 28,762 functions, 298 switch jump tables, split-point closure
- [x] Dolphin signature DB matching (~2,170 matches; OS/GX/DVD/WPAD/AX libraries named)
- [x] C++ code generator (integer, FPU, paired singles + GQR quantisation, CR/XER, SPRs)
- [x] Runtime: 4 GiB mirrored guest address space, fibers for guest threads,
      exception delivery (external + decrementer), timer/scheduler thread
- [x] Hardware: PI, VI (half-line accurate DI interrupts), EXI (RTC/SRAM), SI, DSP (mail
      protocol, ROM/INIT/AX microcodes), AI DMA, Hollywood IPC, CP/PE + GX FIFO parser
- [x] IOS HLE: /dev/di (extracted-file disc + error #001 checks), /dev/fs (host NAND,
      generated SYSCONF/setting.txt), /dev/es, /dev/stm
- [x] D3D11 renderer: vertex loader, TEV/lighting/texgen shader generator, texture decoders,
      EFB copies, XFB present (untested on real frames yet)
- [x] AX audio HLE (port of Dolphin's AXWii) + XAudio2 output (untested)
- [x] WPAD HLE + gamepad motion synthesis (untested)
- [x] First frame on screen
- [x] Title screen / menus / FMV
- [x] Gameplay with a gamepad (movement, jump, slash, guard, Soul Surge)
- [x] Home Button Menu (`hbm_part.rso` runs in the interpreter; IR pointer from
      mouse/right stick; Wii Menu quits, Reset restarts)
- [x] config.ini, 3x internal resolution, 16:9, 60 FPS option (holds 60 in stage 1)
- [x] GX on its own thread (dual-core style), disk shader cache, retrace-wait sleep
- [x] Dolphin-format HD texture packs + texture dumping
- [x] Wii Remote speaker audio, multiple controllers (Battle mode), button bindings
- [x] 60 FPS mode keeps game time correct through menus, FMV and cutscenes
- [x] HOME menu module recompiled (verified by hash at runtime); intro movie 60 fps
- [x] Soak tests: 4 x 15 min + 30 min random input (30 and 60 fps) with no errors;
      mission 1 cleared, results/save/retry/item codex/world map exercised
- [ ] Verify 60 FPS mode in all stage types (needs real play-testing)
- [ ] Controls tuning per stage type (on-rails runs, boss QTEs)

## Performance notes

- Map(WRITE_DISCARD) of a big dynamic VB per draw made the driver stall (~31% of the
  frame): vertex/index data now goes through NO_OVERWRITE ring buffers.
- Texture validation is once per frame per texture; CI index data likewise, the
  palette on every use. Hash64 uses 4 independent lanes.
- Remaining CPU cost is mostly the vertex loader (~20%) and the GX FIFO parser.

## Bugs found by random-input soak tests

`out/soak*.txt` are generated scripts (menus, then ~12 minutes of random stick/button
input); run them with `tools/run_test.ps1 -Seconds 900 -GameArgs "--nobox --input-script
soak.txt --shots 1800 --shots-start 1800"`. The first one cleared mission 1 and found:

- runtime display lists (GXBeginDisplayList redirects the CPU FIFO to RAM) were never
  written to memory;
- the HLSL depth clamp used a hex literal and became an unsigned clamp, so near-plane
  pixels (2D UI) got max depth and full fog;
- the VS/PS interface register of clipPos depended on the texgen count.

## Threading notes

- CPU thread appends write-gather bytes to a 32 MB lock-free ring; the GX thread copies
  them into its FIFO buffer and runs `RunCommands` under `s_gpuLock`.
- Sync points: CP status / FIFO pointer reads and EFB peeks call `GX_SyncGPU()`.
- PE token/finish come from the GX thread: PE flags under `s_peMutex`, PI cause is atomic.

## WPAD globals set by the real WPADInit

Our WPADInit override skips the SDK's SYSCONF caching, so getters that read those
r13 globals need HLE: speaker volume (0x803D6F40), motor enable (0x803D51C0/518C).
Remote speaker: game streams 20-byte Yamaha ADPCM packets (6000 Hz) only after the
ON command completes and an UNMUTE succeeds (state machine around 0x801355A0).

## KPAD pointer notes

- KPAD rejects IR dot pairs whose distance estimate (`kp+1348 / spacing`) is outside
  (`kp+1352` ≈ 0.52, sdata `r13-14728`, which the game sets to 1.0). Dots are spaced
  360 counts apart (≈0.74).
- KPADStatus: pos at 0x20, horizon at 0x34, dist at 0x48, acc_vertical at 0x54,
  dpd_valid_fg at 0x5E. KPAD per-channel state at 0x80781580 + chan*1472.

## Boot log milestones

1. `__start` → crash in BSS memset: decoder ignored the Rc bit of `rlwinm.` (fixed).
2. `RealMode()` uses `rfi` to call a function with translation off → `rfi` = call/return.
3. OS init: IOS version check needs `0x80003188` (set by the apploader) → set to IOS55.
4. DVD "Error #001": out-of-bounds unencrypted reads must fail with 0x052100 and
   `ReportKey` with 0x052000, as on a retail drive.
5. Hang in `GXGetYScaleFactor`: single-precision ops must compute in double and round
   the result (the int→float `0x43300000` trick depends on it) (fixed).

## Build notes

- VS2019 16.10's linker crashes (`LNK1000` in `LIB::Search`) with the Windows SDK
  10.0.26100 import libraries; link against 10.0.19041 (see `tools/dev_build.bat`).
- `tools/dev_build.bat` recompiles only `runtime/` and relinks against the generated objects
  built by CMake (`cmake --build build --target ppcrecomp`).
