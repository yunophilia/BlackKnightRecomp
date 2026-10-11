# Runs the game for a fixed time with the given arguments, then makes sure it is closed.
param(
    [int]$Seconds = 30,
    [string]$GameArgs = "--nobox"
)
$exe = Join-Path $PSScriptRoot "..\out\BlackKnightRecomp.exe"
$dir = Split-Path $exe
Get-Process BlackKnightRecomp -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item (Join-Path $dir "shot_*") -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -ArgumentList $GameArgs -WorkingDirectory $dir -PassThru -WindowStyle Minimized
if (-not $p.WaitForExit($Seconds * 1000)) {
    Stop-Process -Id $p.Id -Force
    "stopped after $Seconds s"
} else {
    "exited with code $($p.ExitCode)"
}
Get-Process BlackKnightRecomp -ErrorAction SilentlyContinue | Stop-Process -Force
