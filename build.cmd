@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem --- locate a Visual Studio / Build Tools installation -------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [!] vswhere.exe not found - install Visual Studio Build Tools with the
    echo     "Desktop development with C++" workload and the Windows SDK.
    exit /b 1
)

set "VSARGS=-latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" %VSARGS%`) do set "VSPATH=%%i"

if not defined VSPATH (
    echo [!] No VC++ toolset found.
    exit /b 1
)

rem (VS 18's vcvars64.bat writes a harmless "vswhere.exe not recognized" to stderr)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if errorlevel 1 exit /b 1

rem --- compile ------------------------------------------------------------
if exist RazerLite.exe del RazerLite.exe

if exist icon.ico (
    rc /nologo /fo app.res app.rc || exit /b 1
    set "RESFILE=app.res"
) else (
    echo [i] icon.ico missing - building without a resource
    set "RESFILE="
)

rem /O1 /Os      optimise for size
rem /GS-         no stack cookie (pulls in the CRT otherwise)
rem /GR- /Zc:*   no RTTI, strict conformance
rem /NODEFAULTLIB + /ENTRY  build without the C runtime
cl /nologo /W4 /O1 /Os /GS- /GR- /Gy /permissive- ^
   /D_UNICODE /DUNICODE /DNDEBUG /DRAZERLITE_NO_CRT ^
   RazerLite.cpp !RESFILE! ^
   /link /SUBSYSTEM:WINDOWS /ENTRY:AppEntryPoint /NODEFAULTLIB ^
   /OPT:REF /OPT:ICF /INCREMENTAL:NO /RELEASE /MAP:RazerLite.map ^
   kernel32.lib user32.lib gdi32.lib shell32.lib advapi32.lib ^
   setupapi.lib hid.lib cfgmgr32.lib winhttp.lib ^
   /OUT:RazerLite.exe || exit /b 1

del /q *.obj app.res 2>nul

for %%F in (RazerLite.exe) do echo [ok] %%~nxF  %%~zF bytes
endlocal
