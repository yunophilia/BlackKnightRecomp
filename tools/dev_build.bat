@echo off
rem Incremental build with the windows-clang preset (configures on first use).
rem The exe goes to out\BlackKnightRecomp.exe.
setlocal
cd /d %~dp0..
where clang-cl >nul 2>nul || set "PATH=C:\Program Files\LLVM\bin;%PATH%"
where ninja >nul 2>nul || set "PATH=%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"
if not exist build\windows-clang\build.ninja (
    cmake --preset windows-clang > out\configure.log 2>&1 || (type out\configure.log & echo CONFIGURE FAILED & exit /b 1)
)
cmake --build --preset windows-clang > out\build.log 2>&1
if errorlevel 1 (
    findstr /C:"error" /C:"FAILED" out\build.log
    echo BUILD FAILED
    exit /b 1
)
echo BUILD OK
