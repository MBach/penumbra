@echo off
rem Builds build\Penumbra-<version>-x64.msi from build\Penumbra.exe, with
rem WiX 5 (a local .NET tool, see dotnet-tools.json).
setlocal
cd /d "%~dp0"
call "%~dp0build.cmd" || exit /b 1

rem File name version = exe version (src\penumbra.rc).
for /f "usebackq delims=" %%v in (`powershell -NoProfile -Command "$v = (Get-Item 'build\Penumbra.exe').VersionInfo; '{0}.{1}.{2}' -f $v.FileMajorPart, $v.FileMinorPart, $v.FileBuildPart"`) do set "VERSION=%%v"
if not defined VERSION (
  echo Could not read the version of build\Penumbra.exe.
  exit /b 1
)
set "MSI=build\Penumbra-%VERSION%-x64.msi"

dotnet tool restore >nul || exit /b 1
dotnet wix build packaging\Penumbra.wxs -arch x64 -o "%MSI%" || exit /b 1
del /q build\*.wixpdb 2>nul
echo Built %MSI%
