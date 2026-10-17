# Runs every mission script in tools/tas/missions at 30 and 60 fps (low impact) with the
# call-counting build, collecting logs/telemetry in out/tas and call counts in out/dumps.
param([string]$Filter = "*", [string]$Exe = "")
$root = Split-Path $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $root "out\BlackKnightRecomp_counts.exe" }
foreach ($s in Get-ChildItem (Join-Path $PSScriptRoot "tas\missions\$Filter.tas")) {
    foreach ($fps in 30, 60) {
        $args = @("-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "tas_run.ps1"), "-Script", $s.FullName, "-Save", "full", "-Exe", $Exe, "-Seconds", "420")
        if ($fps -eq 60) { $args += "-Fps60" }
        $r = & powershell @args
        $calls = Join-Path $root "out\dumps\$($s.BaseName).calls"
        if (Test-Path $calls) { Move-Item -Force $calls (Join-Path $root "out\dumps\$($s.BaseName)_$fps.calls") }
        "$(Get-Date -Format HH:mm) $($r -join ' ')"
    }
}
