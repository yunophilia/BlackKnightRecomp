# TAS scripts (test automation)

Scripted runs drive player 1 with controller input timed by the guest clock and
read game state for telemetry. They never change game code or game data: input goes
through the normal controller path, telemetry only reads guest memory, and the
recompiler's probes only copy a register into `g_tasProbe[]` (see `PROBES` in
`tools/recomp.py`). While a script runs, real controllers are ignored, rumble is off and
the keyboard is only read when the window has focus.

## Running

```
powershell -ExecutionPolicy Bypass -File tools/tas_run.ps1 -Script tools/tas/x.tas [-Fps60] [-Save base|full] [-Full] [-Scale N]
powershell -ExecutionPolicy Bypass -File tools/tas_batch.ps1 [-Filter name*]
python tools/tas/gen_missions.py          # regenerate tools/tas/missions/*.tas
python tools/tas/compare.py               # 30 vs 60 fps report -> out/tas/report.md
```

`tas_run.ps1` copies `out/tasnand/<Save>` to a fresh save folder per run (the player's
own `nand/` is never used), starts the game minimized and collects `out/tas/<tag>.log`
and `<tag>.csv`. By default runs are low impact: muted, 1x internal resolution,
below-normal priority (`-Full` uses the real settings). Game options for scripted runs:
`--tas <script> --telemetry <csv> --nand <dir> --mute --scale <n>`.

Test saves: `out/tasnand/base` (early progress) and `out/tasnand/full` (community PAL
99% save, every mission unlocked).

## Script language

One command per line, `#` starts a comment. Commands run in order; input commands take
guest time.

| command | effect |
|---|---|
| `players N` | controllers the game sees (1 or 2); player 2 takes the `P2:` inputs |
| `filemark` | `file` conditions only count disc reads after this point (default: script start) |
| `wait S` | no input for S seconds |
| `hold S INPUT` | hold INPUT for S seconds |
| `tap INPUT [S]` | hold for S (default 0.1) then release for 0.1 s |
| `mash INPUT S [P]` | press repeatedly for S seconds, period P (default 0.2) |
| `set INPUT` / `set` | background input added to everything that follows / clear it |
| `until COND [and COND] [mash INPUT] [timeout S]` | wait until all conditions hold (default timeout 60 s; a timeout fails the run with exit code 3) |
| `check COND [msg TEXT]` | log PASS/FAIL without waiting |
| `repeat N` ... `end` | loop |
| `log TEXT` | log line and telemetry marker |
| `shot NAME` | screenshot to `out/shots/NAME.bmp` |
| `dump NAME` | MEM1/MEM2 to `out/dumps/NAME.mem1/.mem2` |
| `calls NAME` / `calls -` | write call counts since the last `calls` to `out/dumps/NAME.calls` / reset (call-counting build only) |
| `watch NAME TYPE EXPR` | telemetry column (sampled once per presented frame) |
| `include FILE` | include another script (relative path) |
| `quit [CODE]` | exit the game |

INPUT: `A B X Y LB RB START BACK UP DOWN LEFT RIGHT HOME LS RS` joined with `+`, and
analog values `LX= LY= RX= RY= LT= RT=` (-1..1) and pointer `PX= PY=` (0..1 of the
screen), e.g. `hold 2 LY=1.0+A`. A word prefixed `P2:` is player 2's input, e.g.
`tap A P2:A`. Buttons use the default logical layout (A jump, X
slash, B Wii B, RB guard, ...).

COND: `file TEXT` (a disc file whose path contains TEXT was read since the last
`filemark`) or `TYPE EXPR OP VALUE` with TYPE `u8 s8 u16 s16 u32 s32 f32` and OP
`== != < <= > >=`.

EXPR: hex address, probe `p0`..`p15`, `+`/`-` offsets and `[expr]` to read a pointer,
e.g. `[80803e10]+2050`. Unmapped addresses read as missing values.

## Known addresses (RENE8P)

| what | expression |
|---|---|
| per-step time scale (1.0) | `f32 807FEEE4` (r13 = 0x808066C0) |
| stage timer | `p0`: `u8 +1` minutes, `+2` seconds, `+3` 1/60 s, `f32 +4` |
| rings | `s32 p1` |
| score | `s32 p2+4` |
| Soul gauge | `f32 p3` (max at `+4`) |
| Sonic position | `f32 [8072bfbc]+380/+384/+388` (y up; in normal stages) |
| buttons as the game sees them | `u16 80781582` (0x0800 = A) |

## Call counting

The `windows-clang-counts` preset builds `out/BlackKnightRecomp_counts.exe`, which counts
calls to every main.dol function. Comparing counts per second at 30 and 60 fps
separates code that runs per game logic step (same rate) from code that runs per
rendered frame (twice the rate at 60 fps).
