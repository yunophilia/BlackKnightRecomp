# Dependency on a local Dolphin install

Status as of October 2026. The build used to expect a Dolphin install at
`C:\dolphin_emu\Dolphin-x64`, and the disc had to be extracted with DolphinTool. Neither is
needed any more. This page lists every place that used Dolphin, the options considered, and
what was done.

## Inventory

| What | Used by | When | Without it |
|---|---|---|---|
| `Sys/totaldb.dsy` (signature DB, 1.4 MB) | `tools/build_generated.py` -> `tools/sigdb.py` | Analysis only, on the first run or with `--reanalyze` (the result is cached in `out/functions.json`) | No `sig` names in `out/functions.json`. Only `tools/xref.py` (a dev tool) reads them. `tools/recomp.py` never does, so the generated code is the same either way. |
| `Sys/GC/dsp_coef.bin` (4 KB) | `runtime/audio/ax.cpp` (AX HLE polyphase resampler) | Runtime | Used to fall back to linear resampling, which sounds worse. |
| `DolphinTool.exe extract -g` | README step 1 | Once, before analysis | No `game/extracted/DATA/` (`sys/main.dol`, `files/BLACKKNIGHT.sel`, `tmd.bin`, `ticket.bin`, ...). Both analysis and the runtime need it. |
| Dolphin user dir `Documents/Dolphin Emulator/Load/Textures` | `runtime/gx/hires.cpp` | Runtime, optional | Just one of several places it searches for texture packs. Not a dependency. |

Hardcoded paths: `DSY = r'C:\dolphin_emu\Dolphin-x64\Sys\totaldb.dsy'` in
`tools/build_generated.py`, and `C:\dolphin_emu\Dolphin-x64\Sys\GC\dsp_coef.bin` as the last
search path in `runtime/audio/ax.cpp`. Nothing in the CMake build itself refers to Dolphin.

### What the runtime reads from the disc

`runtime/ios/disc.cpp` builds the decrypted DATA partition from DolphinTool's output
(`sys/boot.bin`, `bi2.bin`, `apploader.img`, `main.dol`, `fst.bin`, and `files/` laid out by
the FST). `dev_es.cpp` and `dev_di.cpp` read `tmd.bin` and `ticket.bin` from the same
directory. The raw `.iso` is used for only two things: the partition table at 0x40000 (to
get the data partition offset) and unencrypted reads (`Disc_ReadRaw`). The runtime never
decrypts anything. Reading the ISO directly would mean decrypting Wii partition clusters
(AES-128-CBC with the title key, which is itself encrypted with the Wii common key).

## Options

### a. Dolphin as a git submodule (like UnleashedRecomp's `tools/XenonRecomp`)

- Size: GitHub reports about 550 MB for the repository. `Externals/` is pulled in as well, and
  `.gitmodules` lists 34 nested submodules. Even a shallow, non-recursive clone is hundreds
  of MB just to get a 1.4 MB and a 4 KB file.
- DolphinTool is not standalone. `Source/Core/DolphinTool/CMakeLists.txt` links `discio` and
  `uicommon`, and `uicommon` pulls in `core`, which is essentially the whole emulator
  (VideoCommon, AudioCommon, the JITs, ...) plus a dozen Externals. On Windows, Dolphin's
  supported build is the MSBuild solution with MSVC. The CMake path on Windows is less
  maintained, and clang-cl is not a supported configuration. Building it would take tens of
  minutes per clean build. I did not try it, because the PC was in use.
- UnleashedRecomp submodules its *own* recompiler tools, which is a different case from
  pulling in a whole emulator for two data files and an extractor.

Verdict: much too heavy for what it buys.

### b. Commit the data files

- `totaldb.dsy` (GPL-2.0-or-later, as part of Dolphin's `Data/Sys`) could be committed. It
  holds checksums and SDK function names, with no Nintendo code. But it only affects
  developer tooling, so committing 1.4 MB of binary is not worth it.
- `dsp_coef.bin` does not need committing at all. Dolphin generates it with
  `docs/DSP/free_dsp_rom/generate_coefs.py`: windowed-sinc filters (Hamming and Kaiser,
  beta = 9pi/4), normalized to 32767, with 22 fixed values patched in for the GBA ucode. It is
  Dolphin's own free replacement for the DSP ROM table, so it carries Dolphin's license
  (GPL-2.0-or-later, compatible with this project's GPL-2.0-only) and is not Nintendo data.
  A C++ port of that script reproduces Dolphin's file bit for bit.

### c. Our own extractor, or reading the ISO directly

- The runtime would need on-the-fly decryption of the DATA partition (0x8000-byte clusters:
  0x400 bytes of hashes, then 0x7C00 bytes of data, with the IV at 0x3D0 of the cluster).
  Windows BCrypt provides AES, so this is roughly 150 lines in `disc.cpp`. The analysis
  scripts would also need `main.dol` and `BLACKKNIGHT.sel`. That means either a small native
  extractor or a Python AES dependency (`cryptography`/`pycryptodome`). Pure-Python AES is
  far too slow for a 4.4 GB disc.
- Every such tool has to embed the Wii common key. Dolphin, Wiimms ISO Tools and nod all
  do. For a public GPL repository this is a policy decision for the project owner, not a
  technical one.
- DolphinTool also reads RVZ/WIA/WBFS/GCZ images. A home-grown reader would be ISO-only
  unless it took on much more work.
- Existing alternatives: [nod](https://github.com/encounter/nod) (`nodtool`, Rust,
  MIT/Apache-2.0, prebuilt Windows binaries, reads ISO/RVZ/WIA/WBFS). Its extraction layout
  would need checking against what `disc.cpp` expects (`DATA/sys`, `DATA/files`, `tmd.bin`,
  `ticket.bin`). Wiimms ISO Tools (`wit extract`, GPL-2.0) is another option.

## What was done

1. **dsp_coef.bin: generate it.** `runtime/audio/dsp_coef.cpp` ports Dolphin's
   `generate_coefs.py`. I checked the output against Dolphin's `Data/Sys/GC/dsp_coef.bin`
   (current master, identical to the 2020 copy in a local install): 0 of 2048 words differ.
   `ax.cpp` still loads a `dsp_coef.bin` next to the exe or in `game/` if one is present
   (for example from a real DSP ROM dump). Otherwise it uses the generated table, so audio
   always gets polyphase resampling. The hardcoded `C:\` path is gone.
2. **totaldb.dsy: not needed.** The 2,175 SDK function names it matched in this game's
   `main.dol` (the only binary the recompiler supports) are recorded in
   `tools/sdk_names.txt`, which `build_generated.py` applies. A full reanalysis with that
   file gives the same functions and names as matching against the database. To rematch,
   pass `--dsy <file>` or set `BKR_TOTALDB`. The names derive from Dolphin's
   GPL-2.0-or-later database, compatible with this project's GPL-2.0-only.
3. **Extraction: our own installer, built on nod (UnleashedRecomp-style).** Dusklight
   (TwilitRealm/dusklight) reads disc images with [nod](https://github.com/encounter/nod)
   through encounter's Aurora/Borealis libraries, Wii discs included. We link nod directly:
   - `runtime/install/install.cpp` opens the image (ISO, RVZ, WIA, WBFS, CISO, GCZ, NFS),
     checks the game ID (`RENE8P`, `RENP8P` or `RENJ8P`) and the SHA-1 of `main.dol` (the
     recompiled code only matches that exact binary), and writes the same layout DolphinTool's `extract -g`
     produces into `game/extracted/DATA`. It also writes `disc/unencrypted.bin`, the first
     0x50000 bytes of the disc. Raw disc reads (`/dev/di` command 0x8D, which never go past
     0x50000) and the partition table now come from that file, so the image is not needed
     after installing. Output goes to `DATA.partial` and is renamed at the end, so an
     interrupted install is never mistaken for a finished one.
   - The game runs this on first start when no data is found: it offers a `.iso` found in
     `game/` or opens a file picker, then shows a progress dialog
     (`runtime/install_ui.cpp`). Scripted runs (`--nobox --skip-launcher`: TAS, smoke tests) never show UI;
     they stop with an error instead.
   - `out/bkr_install <image>` does the same from the command line. Developers need it
     before the first `build_generated.py`, because that reads `sys/main.dol`. CMake now
     configures without `generated/` (it builds only `bkr_install`) and reconfigures by
     itself once `build_generated.py` has filled it.
   - nod comes from its prebuilt `libnod-windows-x86_64.tar.gz` release (v2.0.0-alpha.12,
     pinned by SHA-256), downloaded at configure time, the same package Aurora uses. It is
     linked statically. Its C dependencies were built against the DLL C runtime, so the
     link adds `/NODEFAULTLIB:msvcrt /IGNORE:4217`. The Wii common key lives in nod, not
     in this repository.
   - Licence: nod is "MIT OR Apache-2.0" and is used under MIT, which is compatible with
     GPL-2.0-only. Apache-2.0 alone would not be.
   - I checked nod's output against the DolphinTool extraction: `boot.bin`, `bi2.bin`,
     `apploader.img`, `main.dol`, `fst.bin`, `ticket.bin`, `tmd.bin`, `cert.bin`, `h3.bin`
     and `BLACKKNIGHT.sel` are byte-identical, and so are the raw first 0x50000 bytes.

Data extracted earlier with DolphinTool keeps working. Without `disc/unencrypted.bin`, the
runtime falls back to the ISO for raw reads, as before.
