# Dependency on a local Dolphin install

Status as of October 2026. Before this change the build expected a Dolphin install at
`C:\dolphin_emu\Dolphin-x64`. This page lists every place that used Dolphin, the options for
removing it, and what was done.

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

## Recommendation (implemented)

1. **dsp_coef.bin: generate it.** `runtime/audio/dsp_coef.cpp` ports Dolphin's
   `generate_coefs.py`. I checked the output against Dolphin's `Data/Sys/GC/dsp_coef.bin`
   (current master, identical to the 2020 copy in a local install): 0 of 2048 words differ.
   `ax.cpp` still loads a `dsp_coef.bin` next to the exe or in `game/` if one is present
   (for example from a real DSP ROM dump). Otherwise it uses the generated table, so audio
   always gets polyphase resampling. The hardcoded `C:\` path is gone.
2. **totaldb.dsy: optional and configurable.** `build_generated.py` looks for it in this
   order: `--dsy <file>`, `BKR_TOTALDB`, `game/totaldb.dsy`, then
   `%ProgramFiles%\Dolphin\Sys\totaldb.dsy`. If none exists it skips signature matching and
   says so. It is not vendored, because it only affects developer tooling.
3. **Extraction: keep DolphinTool for now, without installing Dolphin.** DolphinTool comes
   with every Dolphin release, including the portable zip, so nothing has to be installed and
   it handles every disc image format. The README now says so.

With this, the build (`build_generated.py`, `cmake --preset windows-clang`,
`cmake --build`) and the runtime no longer touch any Dolphin file. Only the one-off
extraction step uses a Dolphin binary.

### Possible next step (needs the owner's decision)

To drop DolphinTool as well, read the ISO directly in the runtime (decrypting with BCrypt)
and add a small `--extract` mode, or a separate tiny tool, that writes `main.dol` and
`BLACKKNIGHT.sel` for analysis. This means committing the Wii common key, as Dolphin does,
and it supports ISO only. The other route is to document `nodtool` as an alternative
extractor once its output layout has been checked against `disc.cpp`.
