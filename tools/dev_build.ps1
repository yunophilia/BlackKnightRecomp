# Incremental build with the windows-clang preset (configures on first use).
# Sets up the newest Visual Studio (2022 or later) environment, preferring a standalone
# LLVM install when there is one. The exe goes to out\BlackKnightRecomp.exe.
# Works from any PowerShell window (Windows PowerShell 5.1 or pwsh):
#   powershell -ExecutionPolicy Bypass -File tools\dev_build.ps1
# Exit code 0 and "BUILD OK" on success, 1 and "BUILD FAILED" (or "CONFIGURE FAILED") otherwise.
$ErrorActionPreference = 'Stop'

# The developer environment is only for this build: the calling shell keeps its own
$savedEnv = @{}
Get-ChildItem env: | ForEach-Object { $savedEnv[$_.Name] = $_.Value }
Push-Location (Split-Path -Parent $PSScriptRoot)

function Restore-Environment
{
    Get-ChildItem env: | Where-Object { -not $savedEnv.ContainsKey($_.Name) } |
        ForEach-Object { Remove-Item "env:$($_.Name)" }
    foreach ($name in $savedEnv.Keys) { Set-Item "env:$name" $savedEnv[$name] }
}

# cmd does the redirection: the logs get the tools' bytes as they are (PowerShell 5.1 would
# write UTF-16 and turn stderr lines into errors)
function Invoke-Logged([string]$command, [string]$log)
{
    cmd /d /c "$command > $log 2>&1"
    return $LASTEXITCODE
}

$code = 1
try
{
    if (-not $env:VSCMD_VER)
    {
        # where the Visual Studio Installer puts it, else a vswhere.exe on PATH
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path $vswhere))
        {
            $onPath = Get-Command vswhere.exe -CommandType Application -ErrorAction SilentlyContinue |
                Select-Object -First 1
            if ($onPath) { $vswhere = $onPath.Source }
        }
        $vsDir = $null
        if (Test-Path $vswhere)
        {
            $vsDir = & $vswhere -latest -products * -version '[17.0,)' `
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
                Select-Object -First 1
        }
        if (-not $vsDir)
        {
            Write-Host 'Visual Studio 2022 or newer with the C++ workload is required'
            exit 1
        }
        $env:PATH = "$(Split-Path -Parent $vswhere);$env:PATH"   # VS's own scripts call vswhere
        Import-Module (Join-Path $vsDir 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
        Enter-VsDevShell -VsInstallPath $vsDir -SkipAutomaticLocation -Arch amd64 -HostArch amd64 `
            -DevCmdArguments '-no_logo' | Out-Null
    }
    # Visual Studio's CMake and Ninja, ahead of any older standalone ones
    $ide = Join-Path $env:VSINSTALLDIR 'Common7\IDE\CommonExtensions\Microsoft\CMake'
    $env:PATH = "$ide\CMake\bin;$ide\Ninja;$env:PATH"
    $llvm = Join-Path $env:ProgramFiles 'LLVM\bin'
    if (Test-Path (Join-Path $llvm 'clang-cl.exe')) { $env:PATH = "$llvm;$env:PATH" }

    New-Item -ItemType Directory -Force out | Out-Null
    if (-not (Test-Path build\windows-clang\CMakeCache.txt))
    {
        if ((Invoke-Logged 'cmake --preset windows-clang' 'out\configure.log') -ne 0)
        {
            Get-Content out\configure.log
            Write-Host 'CONFIGURE FAILED'
            exit 1
        }
    }
    if ((Invoke-Logged 'cmake --build --preset windows-clang' 'out\build.log') -ne 0)
    {
        Select-String -Path out\build.log -Pattern 'error', 'FAILED' -SimpleMatch -CaseSensitive |
            ForEach-Object { $_.Line }
        Write-Host 'BUILD FAILED'
        exit 1
    }
    Write-Host 'BUILD OK'
    $code = 0
}
finally
{
    Pop-Location
    Restore-Environment
}
exit $code
