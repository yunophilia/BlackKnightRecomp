# Runs every mission script in tools/tas/missions at 30 and 60 fps (low impact) with the
# call-counting build, collecting logs/telemetry in out/tas and call counts in out/dumps.
param([string]$Filter = "*", [string]$Exe = "", [switch]$Resume)   # -Resume skips runs that already have call counts
$root = Split-Path $PSScriptRoot
# two batches would close each other's test game (tas_run.ps1 closes running copies of the exe)
$others = Get-CimInstance Win32_Process | Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -match 'tas_batch\.ps1' -and $_.Name -eq 'powershell.exe' }
if ($others) { throw "another TAS batch is already running (PID $(($others | ForEach-Object ProcessId) -join ', '))" }
if (-not $Exe) { $Exe = Join-Path $root "out\BlackKnightRecomp_counts.exe" }
foreach ($s in Get-ChildItem (Join-Path $PSScriptRoot "tas\missions\$Filter.tas")) {
    foreach ($fps in 30, 60) {
        if ($Resume -and (Test-Path (Join-Path $root "out\dumps\$($s.BaseName)_$fps.calls"))) { continue }
        $args = @("-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "tas_run.ps1"), "-Script", $s.FullName, "-Save", "full", "-Exe", $Exe, "-Seconds", "420")
        if ($fps -eq 60) { $args += "-Fps60" }
        $r = & powershell @args
        $calls = Join-Path $root "out\dumps\$($s.BaseName).calls"
        if (Test-Path $calls) { Move-Item -Force $calls (Join-Path $root "out\dumps\$($s.BaseName)_$fps.calls") }
        "$(Get-Date -Format HH:mm) $($r -join ' ')"
    }
}
