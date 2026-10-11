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
- [ ] Verify 60 FPS mode across cutscenes and all stage types
- [ ] Controls tuning per stage type (on-rails runs, boss QTEs)

## Performance notes

- Map(WRITE_DISCARD) of a big dynamic VB per draw made the driver stall (~31% of the
  frame): vertex/index data now goes through NO_OVERWRITE ring buffers.
- Texture validation is once per frame per texture; CI index data likewise, the
  palette on every use. Hash64 uses 4 independent lanes.
- Remaining CPU cost is mostly the vertex loader (~20%) and the GX FIFO parser.

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
