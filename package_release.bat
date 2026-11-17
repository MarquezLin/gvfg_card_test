@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "PACKAGE_PAUSE=1"
for %%A in (%*) do (
    if /i "%%~A"=="/nopause" set "PACKAGE_PAUSE=0"
    if /i "%%~A"=="/source-runtime" set "GVFG_BUILD_SOURCE_RUNTIME=1"
)
if not defined QT_ROOT set "QT_ROOT=C:\Qt\6.10.2\msvc2022_64"
if not defined CMAKE_EXE set "CMAKE_EXE=C:\Qt\Tools\CMake_64\bin\cmake.exe"
if not defined NINJA_EXE set "NINJA_EXE=C:\Qt\Tools\Ninja\ninja.exe"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VS_ROOT if exist "%VSWHERE%" for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%I"
set "RESULT=1"
if not defined VS_ROOT goto missing_vs
if not exist "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" goto missing_vs
call "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 goto finish
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0package_release.ps1"
set "RESULT=%ERRORLEVEL%"
goto finish

:missing_vs
echo ERROR: Visual Studio C++ x64 build tools not found. Set VS_ROOT if installed elsewhere.

:finish
if "%PACKAGE_PAUSE%"=="1" pause
exit /b %RESULT%
