# Collects a playable build into dist/BlackKnightRecomp (no game data is included).
#   powershell -ExecutionPolicy Bypass -File tools/package.ps1
# The player copies their extracted disc into dist/BlackKnightRecomp/game/ (see README).
$root = Split-Path $PSScriptRoot
$dist = Join-Path $root "dist\BlackKnightRecomp"
$exe = Join-Path $root "out\BlackKnightRecomp.exe"
if (-not (Test-Path $exe)) { throw "build first (cmake --build --preset windows-clang)" }
New-Item -ItemType Directory -Force $dist | Out-Null
New-Item -ItemType Directory -Force (Join-Path $dist "game") | Out-Null
Copy-Item $exe $dist -Force
Copy-Item (Join-Path $root "README.md") $dist -Force
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
Put your own game files here:
  extracted\DATA\...                        (DolphinTool extract -i RENE8P.iso -o extracted -g)
  RENE8P_Sonic_and_the_Black_Knight.iso     (used for raw disc reads)
  textures\RENE8P\...                       (optional, Dolphin-format HD texture pack)
"@ | Set-Content (Join-Path $dist "game\PUT_GAME_FILES_HERE.txt")
"packaged to $dist"
