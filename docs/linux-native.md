# Native Linux / SteamOS: assessment

Status: assessment only, nothing here is implemented. Written for issue #5 (black screen on
the Steam Deck under Proton). The Proton cause is fixed separately; this document answers
whether a native Linux build is worth doing as well.

## Where things stand

* The recompiled game code (`generated/`, the `ppcrecomp` target) already builds with clang
  on Linux (`linux-clang` preset). It is plain C++ with no OS calls.
* The runtime (about 15k lines in `runtime/`) is Windows-only. Most of it is portable C++
  (GX command processing, TEV shader generation, AX audio HLE, IOS/HW emulation, KPAD/WPAD
  HLE, config, TAS); the Windows-specific parts are concentrated in a few places:

| Area | Files | Windows API used | Native replacement |
|---|---|---|---|
| Guest memory | `kernel/memory.cpp` | `VirtualAlloc2` placeholders + `MapViewOfFile3` (MEM1/MEM2 mirrors) | `memfd_create` + `mmap(MAP_FIXED)`, simpler than the Windows version |
| Guest threads | `kernel/threads.cpp` | Fibers (`OSSaveContext`/`OSLoadContext` HLE) | a small assembly context switch, or `ucontext` (slower) |
| Crash reports | `kernel/crash.cpp` | vectored exception handler, dbghelp | `sigaction` (SIGSEGV/SIGFPE/SIGILL) + `backtrace`/libunwind |
| Window | `gx/video.cpp` | Win32 window, borderless fullscreen | SDL3 |
| Renderer | `gx/renderer.cpp` (2k lines), `gx/video.cpp`, `gx/hires.cpp` | Direct3D 11, runtime HLSL via `D3DCompile`, DXGI swap chain | see below |
| Audio out | `audio/audio_out.cpp` (165 lines) | XAudio2 | SDL3 audio stream |
| Input | `input/gamepad.cpp` | XInput, `GetAsyncKeyState`, Win32 mouse | SDL3 gamepad/keyboard/mouse (Steam Input shows up as an SDL gamepad) |
| Launcher, overlay | `ui/*.cpp`, `install_ui.cpp` | ImGui Win32 + DX11 backends, `IFileOpenDialog`, `MessageBox` | ImGui SDL3 backend; `SDL_ShowOpenFileDialog`, `SDL_ShowMessageBox` |
| Installer | `install/` | nod prebuilt for Windows | nod's Linux build (nod is Rust and cross-platform) |
| Paths, timers | various | `GetModuleFileNameW`, `_wfopen`, `timeBeginPeriod`, power throttling | `/proc/self/exe`, `std::filesystem`, `clock_nanosleep`; saves and config under `$XDG_DATA_HOME` when the exe directory is read-only (AppImage/Flatpak) |

## The renderer: three ways

1. **dxvk-native + vkd3d-shader.** Keep the D3D11 renderer and link
   [DXVK's native build](https://github.com/doitsujin/dxvk) (D3D11 on Vulkan, with SDL
   windows; used by several native Linux ports), and compile the generated HLSL with
   libvkd3d-shader, the HLSL compiler Wine/Proton use. Issue #5 showed that this compiler
   has gaps (it had no `isnan`), but after the fix it compiles every shader the game has
   recorded (164 vertex, 2641 pixel shader keys) and renders correctly under Wine + DXVK.
   Least work, and it keeps one renderer for both platforms. Risk: two third-party
   libraries in the shipped build, and their HLSL/D3D11 coverage decides what we can use.
2. **SDL3 GPU API.** One modern API for Vulkan/D3D12/Metal. The shader generator would have
   to produce SPIR-V (HLSL through DXC or SDL_shadercross at runtime, or a GLSL/SPIR-V
   generator). The renderer is rewritten against a different resource model (no D3D11-style
   immediate context, explicit render passes and copy passes); EFB copies, peeks and logic
   ops need rework.
3. **Plain Vulkan.** Most control and the most work (descriptor management, pipeline cache,
   synchronisation), plus a SPIR-V path for the shaders as in option 2.

## Rough effort

For someone who knows this codebase, with a Deck or Linux PC to test on:

| Work | dxvk-native route | SDL3 GPU / Vulkan route |
|---|---|---|
| Platform layer (memory, fibers, crash handler, paths, timers) | 1-2 weeks | 1-2 weeks |
| SDL3 window, input, audio | 1-2 weeks | 1-2 weeks |
| Renderer | 1-2 weeks | 4-8 weeks |
| Launcher/overlay/installer on SDL3 + ImGui | 1 week | 1 week |
| Packaging (AppImage, CI build), Deck testing | 1 week | 1 week |
| **Total** | **about 5-8 weeks** | **about 8-14 weeks** |

Every later change then has to be tested on two platforms (input, timing, audio latency
and graphics behave differently).

## Packaging

* **AppImage** or a plain `.tar.gz`: simplest for a non-Steam game on the Deck; the user
  adds it to Steam like the Windows exe. Writable data (installed game files, saves,
  `config.ini`, shader cache) must then live outside the image, for example
  `~/.local/share/BlackKnightRecomp`.
* **Flatpak**: good desktop integration, but sandboxing complicates picking the disc image
  from anywhere and storing 3.5 GB of game data; more maintenance (Flathub review).

## Recommendation

**Proton is enough for now.** That is the usual path for Windows-only recomps on the Deck,
and with the issue #5 fixes the Windows build runs the same code under Proton:

* the release ships precompiled shader bytecode, so DXVK gets the same shaders as on
  Windows and Wine's HLSL compiler is only needed for shaders the bundle does not have;
* the generated HLSL no longer uses what Wine's compiler is missing;
* the log now says when it runs under Wine and reports shader and D3D errors, so a Deck
  user's log points at the cause.

A native port would remove the dependency on Proton and its compiler, but the benefit for
players is small next to 1.5-3 months of work and a second platform to maintain. Revisit it
if Proton keeps producing bugs we cannot fix from the Windows side, or if a contributor with
a Deck wants to own it. If it happens, take the dxvk-native route first: it reuses the
renderer, and the platform layer it needs (memory, fibers, SDL3 window/input/audio) is
needed by every route.
