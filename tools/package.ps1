# Collects a playable build into dist/BlackKnightRecomp (no game data is included).
#   powershell -ExecutionPolicy Bypass -File tools/package.ps1
# The player copies their extracted disc into dist/BlackKnightRecomp/game/ (see README).
$root = Split-Path $PSScriptRoot
$dist = Join-Path $root "dist\BlackKnightRecomp"
$exe = Join-Path $root "out\BlackKnightRecomp.exe"
if (-not (Test-Path $exe)) { throw "build first (tools/dev_build.bat)" }
New-Item -ItemType Directory -Force $dist | Out-Null
New-Item -ItemType Directory -Force (Join-Path $dist "game") | Out-Null
Copy-Item $exe $dist -Force
Copy-Item (Join-Path $root "README.md") $dist -Force
@"
Put your own game files here:
  extracted\DATA\...                        (DolphinTool extract -i RENE8P.iso -o extracted -g)
  RENE8P_Sonic_and_the_Black_Knight.iso     (used for raw disc reads)
  dsp_coef.bin                              (optional, from Dolphin\Sys\GC; better audio resampling)
  textures\RENE8P\...                       (optional, Dolphin-format HD texture pack)
"@ | Set-Content (Join-Path $dist "game\PUT_GAME_FILES_HERE.txt")
"packaged to $dist"
