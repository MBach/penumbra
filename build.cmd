@echo off
rem Builds build\Penumbra.exe with the MSVC toolchain (x64, static CRT).
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
  echo MSVC C++ tools not found.
  exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1

cd /d "%~dp0"
if not exist build mkdir build
rc /nologo /fo build\penumbra.res src\penumbra.rc || exit /b 1
cl /nologo /std:c++17 /utf-8 /O1 /MT /W4 /EHsc /permissive- /DUNICODE /D_UNICODE ^
   src\penumbra.cpp build\penumbra.res /Fo:build\ /Fe:build\Penumbra.exe ^
   /link /SUBSYSTEM:WINDOWS || exit /b 1
echo Built build\Penumbra.exe
