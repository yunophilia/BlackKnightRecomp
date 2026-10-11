@echo off
rem Fast iteration build: recompiles runtime/ and relinks against the already
rem compiled recompiled-code objects in build\ppcrecomp.dir\Release.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
rem The 10.0.26100 SDK import libraries crash the VS2019 16.10 linker (LNK1000), use 19041.
set LIB=%VCToolsInstallDir%lib\x64;C:\Program Files (x86)\Windows Kits\10\lib\10.0.19041.0\um\x64;C:\Program Files (x86)\Windows Kits\10\lib\10.0.19041.0\ucrt\x64
cd /d %~dp0..
if not exist out\rtobj mkdir out\rtobj
set SRCS=
for /R runtime %%f in (*.cpp) do call set SRCS=%%SRCS%% "%%f"
cl /nologo /c /MP /O2 /MT /EHsc /std:c++latest /wd4611 /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /Iruntime /Iruntime\cpu /Igenerated %SRCS% /Foout\rtobj\ > out\cl.log 2>&1
findstr /R /C:"error C[0-9]" /C:"fatal error" out\cl.log
if not errorlevel 1 (
    echo COMPILE FAILED
    exit /b 1
)
dir /b /s out\rtobj\*.obj > out\link_dev.rsp
dir /b /s build\ppcrecomp.dir\Release\*.obj >> out\link_dev.rsp
link /nologo /OUT:out\BlackKnightRecomp.exe /MACHINE:X64 /SUBSYSTEM:CONSOLE /STACK:268435456 /DEBUG:FASTLINK /INCREMENTAL:NO @out\link_dev.rsp d3d11.lib dxgi.lib d3dcompiler.lib xinput.lib winmm.lib ole32.lib xaudio2.lib kernel32.lib user32.lib gdi32.lib dbghelp.lib > out\link.log 2>&1
if errorlevel 1 (
    type out\link.log
    echo LINK FAILED
    exit /b 1
)
echo BUILD OK
