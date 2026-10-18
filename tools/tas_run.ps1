# Runs a TAS script (docs/tas.md) on a fresh copy of a test save folder and collects the
# log and telemetry in out/tas/<Tag>.log / .csv. The window starts minimized; scripted
# input never rumbles the controller.
#   powershell -ExecutionPolicy Bypass -File tools/tas_run.ps1 -Script tools/tas/x.tas [-Fps60] [-Save base] [-Tag name] [-Seconds 600]
param(
    [Parameter(Mandatory)][string]$Script,
    [switch]$Fps60,
    [string]$Save = "base",           # out/tasnand/<Save> is copied for each run
    [string]$Tag = "",
    [int]$Seconds = 600,
    [switch]$Full,
    [int]$Scale = 1,                 # internal resolution for low-impact runs                    # real settings; default is low impact (muted, 1x, below-normal priority)
    [string]$Exe = "",
    [string]$ExtraArgs = ""           # more game options, e.g. "--watch 80753efc"
)
$root = Split-Path $PSScriptRoot
$out = Join-Path $root "out"
if (-not $Exe) { $Exe = Join-Path $out "BlackKnightRecomp.exe" }
if (-not $Tag) { $Tag = [IO.Path]::GetFileNameWithoutExtension($Script) + $(if ($Fps60) { "_60" } else { "_30" }) }
$tasDir = Join-Path $out "tas"
New-Item -ItemType Directory -Force $tasDir | Out-Null
$saveSrc = Join-Path $out "tasnand\$Save"
if (-not (Test-Path $saveSrc)) { throw "missing test save folder $saveSrc" }
$nand = Join-Path $out "tasnand\run_$Tag"
if (Test-Path $nand) { Remove-Item -Recurse -Force $nand }
Copy-Item -Recurse $saveSrc $nand
$csv = Join-Path $tasDir "$Tag.csv"
if (-not (Test-Path $Script)) { throw "TAS script not found: $Script" }
$scriptPath = (Resolve-Path $Script).Path
$gameArgs = "--nobox --tas `"$scriptPath`" --telemetry `"$csv`" --nand `"$nand`""
if ($Fps60) { $gameArgs += " --60fps" }
if (-not $Full) { $gameArgs += " --mute --scale $Scale" }
if ($ExtraArgs) { $gameArgs += " $ExtraArgs" }

Get-Process BlackKnightRecomp* -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Resolve-Path $Exe).Path } | Stop-Process -Force
$runStart = Get-Date
$p = Start-Process -FilePath $Exe -ArgumentList $gameArgs -WorkingDirectory (Split-Path $Exe) -PassThru -WindowStyle Minimized
if (-not $Full) { try { $p.PriorityClass = "BelowNormal" } catch {} }
$code = $null
if ($p.WaitForExit($Seconds * 1000)) { $code = $p.ExitCode } else { Stop-Process -Id $p.Id -Force }
Get-Process BlackKnightRecomp* -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Resolve-Path $Exe).Path } | Stop-Process -Force
Start-Sleep -Milliseconds 300
Copy-Item (Join-Path (Split-Path $Exe) "BlackKnightRecomp.log") (Join-Path $tasDir "$Tag.log") -Force
# keep this run's screenshots apart (scripts use the same names at 30 and 60 fps)
$shotDir = Join-Path $out "shots"
if (Test-Path $shotDir) {
    $mine = Get-ChildItem $shotDir -Filter *.bmp | Where-Object { $_.LastWriteTime -ge $runStart }
    if ($mine) {
        $dst = Join-Path $tasDir "shots\$Tag"
        New-Item -ItemType Directory -Force $dst | Out-Null
        $mine | Move-Item -Destination $dst -Force
    }
}
$log = Get-Content (Join-Path $tasDir "$Tag.log")
$fails = $log | Where-Object { $_ -match "TAS: FAIL|\[E\]" }
$result = if ($code -eq 0 -and -not $fails) { "OK" } elseif ($code -eq $null) { "TIMEOUT" } else { "FAILED (exit $code)" }
"$Tag : $result"
$fails | Select-Object -First 5
