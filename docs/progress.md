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
- [ ] First frame on screen
- [ ] Title screen / menus
- [ ] Gameplay, controls tuning (swing gestures)
- [ ] Home Button Menu (`hbm_part.rso` is a separate module, not yet recompiled)
- [ ] Widescreen/high-res polish, 60 FPS option

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
