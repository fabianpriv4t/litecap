@echo off
rem Baut LiteCap (Kern + Einstellungs-GUI). Aufruf: build.cmd [release^|debug]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=release
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set VSDIR=%%i
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if "%VCPKG_ROOT%"=="" set VCPKG_ROOT=%VSDIR%\VC\vcpkg
cd /d "%~dp0"

rem 1) Kern (C++) – beim ersten Mal baut vcpkg FFmpeg (20-40 min)
if not exist build\CMakeCache.txt (
  cmake --preset x64 || exit /b 1
)
cmake --build --preset %CONFIG% || exit /b 1

rem 2) Einstellungs-GUI (.NET 9) neben litecap.exe
if /i "%CONFIG%"=="debug" (set OUTDIR=build\Debug) else (set OUTDIR=build\Release)
where dotnet >nul 2>nul || (echo dotnet nicht gefunden - GUI wird uebersprungen & exit /b 0)
dotnet publish settings\LiteCapSettings.csproj -c Release -r win-x64 --self-contained false ^
  -p:PublishSingleFile=true -o %OUTDIR% --nologo -v quiet || exit /b 1
echo.
echo Fertig: %OUTDIR%\litecap.exe
endlocal
