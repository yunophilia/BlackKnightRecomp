# Regression smoke test: boots through the menus into mission 1 with a scripted input
# run (tools/scripts/mission1.txt), then checks the log and screenshots.
#   powershell -ExecutionPolicy Bypass -File tools/smoke_test.ps1 [-Fps60]
# Needs the game files in game/ and a built out/BlackKnightRecomp.exe.
param([switch]$Fps60)
$root = Split-Path $PSScriptRoot
$out = Join-Path $root "out"
$script = Join-Path $PSScriptRoot "scripts\mission1.txt"
if (-not (Test-Path $script)) { throw "missing $script" }
$args = "--nobox --input-script `"$script`" --shots 600 --shots-start 600"
if ($Fps60) { $args = "--nobox --60fps --input-script `"$script`" --shots 1200 --shots-start 1200" }
& (Join-Path $PSScriptRoot "run_test.ps1") -Seconds 200 -GameArgs $args | Out-Null
Start-Sleep 2
$log = Get-Content (Join-Path $out "BlackKnightRecomp.log")
$fail = @()

$errors = $log | Where-Object { $_ -like "[[]E]*" }
if ($errors) { $fail += "$($errors.Count) error lines, first: $($errors[0])" }

foreach ($m in @("XAudio2 initialised", "WPAD: controller 0 connected", "HOME menu module")) {
    if (-not ($log | Where-Object { $_ -like "*$m*" })) { $fail += "missing log line: $m" }
}

# frame rate over the second half of the run (gameplay)
$fps = $log | Select-String "status: ([0-9.]+) fps" | ForEach-Object { [double]$_.Matches[0].Groups[1].Value }
$tail = $fps | Select-Object -Last ([int]($fps.Count / 2))
$target = $(if ($Fps60) { 55 } else { 28 })
$low = $tail | Where-Object { $_ -lt $target }
if ($low.Count -gt 2) { $fail += "frame rate below $target in $($low.Count) status lines" }

# screenshots must not be blank
Add-Type -AssemblyName System.Drawing
$shots = Get-ChildItem $out -Filter "shot_*.bmp"
if ($shots.Count -lt 5) { $fail += "only $($shots.Count) screenshots" }
foreach ($s in $shots | Select-Object -Last 3) {
    $bmp = [System.Drawing.Bitmap]::FromFile($s.FullName)
    $sum = 0
    for ($y = 0; $y -lt $bmp.Height; $y += 37) { for ($x = 0; $x -lt $bmp.Width; $x += 37) { $c = $bmp.GetPixel($x, $y); $sum += $c.R + $c.G + $c.B } }
    $bmp.Dispose()
    if ($sum -lt 1000) { $fail += "$($s.Name) is blank" }
}

if ($fail) { "SMOKE TEST FAILED"; $fail | ForEach-Object { "  $_" }; exit 1 }
"SMOKE TEST PASSED ($($shots.Count) screenshots, min gameplay fps $([math]::Round(($tail | Measure-Object -Minimum).Minimum, 1)))"
