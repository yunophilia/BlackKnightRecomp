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
# licences of the libraries built into the exe (collected by the build)
New-Item -ItemType Directory -Force (Join-Path $dist "licenses") | Out-Null
Copy-Item -Force (Join-Path $root "out\licenses\*") (Join-Path $dist "licenses")
# Ship a precompiled shader cache: the GX states recorded in data/shadercache (plus any the
# development build has seen in out/shadercache), compiled here with Microsoft's HLSL
# compiler. A fresh install then does not compile on first play, and under Wine/Proton the
# game does not depend on Wine's own HLSL compiler for these shaders.
$dstCache = Join-Path $dist "shadercache"
New-Item -ItemType Directory -Force $dstCache | Out-Null
Get-ChildItem $dstCache -Filter *.cso | Remove-Item   # blobs of an earlier package; rebuilt below
foreach ($k in "vs_keys.bin", "ps_keys.bin") {
    $bytes = [byte[]]@()
    foreach ($src in (Join-Path $root "data\shadercache\$k"), (Join-Path $root "out\shadercache\$k"), (Join-Path $dstCache $k)) {
        if (Test-Path $src) { $bytes += [IO.File]::ReadAllBytes($src) }
    }
    [IO.File]::WriteAllBytes((Join-Path $dstCache $k), $bytes)   # de-duplicated by the precompile step
}
& $exe --precompile-shaders $dstCache
if ($LASTEXITCODE -ne 0) { throw "shader precompilation failed" }
@"
You don't need to put anything here. Start BlackKnightRecomp.exe and select your own disc
image (USA, Europe or Japan; ISO, RVZ, WIA or WBFS) in the file dialog, wherever it is on your PC.
The launcher installs the game data into this folder; the image is not needed afterwards.

Optional, if you have one:
  textures\RENE8P\...                       (Dolphin-format HD texture pack; the folder is
                                            named by the game ID: RENE8P, RENP8P or RENJ8P)
  mods\<mod name>\...                       (file-replacement mods; or get them in the game:
                                            Settings > Mods > Get mods)
"@ | Set-Content (Join-Path $dist "game\ABOUT_THIS_FOLDER.txt")
"packaged to $dist"
