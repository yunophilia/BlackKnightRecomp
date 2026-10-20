# Collects a playable build into dist/BlackKnightRecomp (no game data is included).
#   powershell -ExecutionPolicy Bypass -File tools/package.ps1
# On first run the exe asks for the player's disc image and installs the data into game/.
$root = Split-Path $PSScriptRoot
$dist = Join-Path $root "dist\BlackKnightRecomp"
$exe = Join-Path $root "out\BlackKnightRecomp.exe"
if (-not (Test-Path $exe)) { throw "build first (cmake --build --preset windows-clang)" }
New-Item -ItemType Directory -Force $dist | Out-Null
New-Item -ItemType Directory -Force (Join-Path $dist "game") | Out-Null
Copy-Item $exe $dist -Force
Copy-Item (Join-Path $root "README.md") $dist -Force
Copy-Item (Join-Path $root "LICENSE") $dist -Force   # GPL-2.0: the licence travels with the binary
# Seed the shader cache from the development build so a fresh package does not compile
# everything on first play (existing entries in dist are kept).
$srcCache = Join-Path $root "out\shadercache"
if (Test-Path $srcCache) {
    $dstCache = Join-Path $dist "shadercache"
    New-Item -ItemType Directory -Force $dstCache | Out-Null
    Get-ChildItem $srcCache -Filter *.cso | Where-Object { -not (Test-Path (Join-Path $dstCache $_.Name)) } | Copy-Item -Destination $dstCache
    foreach ($k in "vs_keys.bin", "ps_keys.bin") {
        $s = Join-Path $srcCache $k
        $d = Join-Path $dstCache $k
        if (Test-Path $s) {
            # appended keys are de-duplicated by the game at startup
            $bytes = [IO.File]::ReadAllBytes($s)
            if (Test-Path $d) { $bytes = [IO.File]::ReadAllBytes($d) + $bytes }
            [IO.File]::WriteAllBytes($d, $bytes)
        }
    }
}
@"
The game data is installed here on first run: start BlackKnightRecomp.exe and select your
own disc image (PAL, RENE8P; ISO, RVZ, WIA or WBFS). Optional:
  textures\RENE8P\...                       (Dolphin-format HD texture pack)
"@ | Set-Content (Join-Path $dist "game\PUT_GAME_FILES_HERE.txt")
"packaged to $dist"
