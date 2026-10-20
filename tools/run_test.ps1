# Runs the game for a fixed time with the given arguments, then makes sure it is closed.
param(
    [int]$Seconds = 30,
    [string]$GameArgs = "--nobox --skip-launcher",
    [string]$Exe = ""
)
if (-not $Exe) { $Exe = Join-Path $PSScriptRoot "..\out\BlackKnightRecomp.exe" }
$exe = $Exe
$dir = Split-Path $exe
Get-Process BlackKnightRecomp* -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Resolve-Path $Exe).Path } | Stop-Process -Force
Remove-Item (Join-Path $dir "shot_*") -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -ArgumentList $GameArgs -WorkingDirectory $dir -PassThru -WindowStyle Minimized
if (-not $p.WaitForExit($Seconds * 1000)) {
    $p.Refresh()
    $cpu = $p.TotalProcessorTime.TotalSeconds
    Stop-Process -Id $p.Id -Force
    "stopped after $Seconds s, {0:N1} s CPU time" -f $cpu
} else {
    "exited with code $($p.ExitCode)"
}
Get-Process BlackKnightRecomp* -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Resolve-Path $Exe).Path } | Stop-Process -Force
