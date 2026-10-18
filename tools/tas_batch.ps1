# Runs every mission script in tools/tas/missions at 30 and 60 fps (low impact) with the
# call-counting build, collecting logs/telemetry in out/tas and call counts in out/dumps.
# -Resume skips runs that already have call counts; -Full uses the real settings (3x, audio,
# normal priority) with the normal exe, e.g. -Full -Rates 60 for the final performance pass
param([string]$Filter = "*", [string]$Exe = "", [switch]$Resume, [switch]$Full, [int[]]$Rates = @(30, 60))
$root = Split-Path $PSScriptRoot
# two batches would close each other's test game (tas_run.ps1 closes running copies of the exe)
$others = Get-CimInstance Win32_Process | Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -match 'tas_batch\.ps1' -and $_.Name -eq 'powershell.exe' }
if ($others) { throw "another TAS batch is already running (PID $(($others | ForEach-Object ProcessId) -join ', '))" }
if (-not $Exe) { $Exe = Join-Path $root $(if ($Full) { "out\BlackKnightRecomp.exe" } else { "out\BlackKnightRecomp_counts.exe" }) }
foreach ($s in Get-ChildItem (Join-Path $PSScriptRoot "tas\missions\$Filter.tas")) {
    foreach ($fps in $Rates) {
        if ($Resume -and (Test-Path (Join-Path $root "out\dumps\$($s.BaseName)_$fps.calls"))) { continue }
        $args = @("-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "tas_run.ps1"), "-Script", $s.FullName, "-Save", "full", "-Exe", $Exe, "-Seconds", "420")
        if ($fps -eq 60) { $args += "-Fps60" }
        if ($Full) { $args += @("-Full", "-Tag", "$($s.BaseName)_${fps}_full") }
        $r = & powershell @args
        $calls = Join-Path $root "out\dumps\$($s.BaseName).calls"
        if (Test-Path $calls) { Move-Item -Force $calls (Join-Path $root "out\dumps\$($s.BaseName)_$fps.calls") }
        "$(Get-Date -Format HH:mm) $($r -join ' ')"
    }
}
