@echo off
setlocal

cd /d "%~dp0"

rem Locate the newest installed Visual Studio developer environment.
set "VSDEVCMD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VSDEVCMD=%%I\Common7\Tools\VsDevCmd.bat"
    )
)

if not defined VSDEVCMD (
    set "VSDEVCMD=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
)

if not exist "%VSDEVCMD%" (
    echo ERROR: Visual Studio Developer Command Prompt not found.
    echo Install the MSVC C++ workload or edit VSDEVCMD in deploy.bat.
    pause
    exit /b 1
)

call "%VSDEVCMD%" -arch=x64
if errorlevel 1 exit /b 1

echo.
echo === Configure Release ===
cmake -S . -B out\build\release -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto :error

echo.
echo === Build Release ===
cmake --build out\build\release
if errorlevel 1 goto :error

if not exist "out\build\release\OpenScopePcmEncoder.exe" (
    echo ERROR: Release executable was not created.
    goto :error
)

echo.
echo === Clean deploy directory ===
if exist out\deploy (
    rmdir /s /q out\deploy
)
mkdir out\deploy\bin
if errorlevel 1 goto :error

echo.
echo === Create deploy ===
copy /y "out\build\release\OpenScopePcmEncoder.exe" "out\deploy\bin\" >nul
if errorlevel 1 goto :error

for %%F in ("out\build\release\*.dll") do (
    if exist "%%~fF" copy /y "%%~fF" "out\deploy\bin\" >nul
)

if exist "out\build\release\qt.conf" (
    copy /y "out\build\release\qt.conf" "out\deploy\bin\" >nul
)

if exist "out\build\release\platforms" (
    robocopy "out\build\release\platforms" "out\deploy\bin\platforms" /E /NFL /NDL /NJH /NJS /NP
    if errorlevel 8 goto :error
) else (
    echo ERROR: Qt platforms directory not found in Release build.
    goto :error
)

if not exist "out\deploy\bin\platforms\qwindows.dll" (
    echo ERROR: qwindows.dll missing from deploy.
    goto :error
)

echo.
echo === OpenScope PCM Encoder deploy ready ===
echo %CD%\out\deploy\bin
echo.
pause
exit /b 0

:error
echo.
echo *** DEPLOY FAILED ***
echo.
pause
exit /b 1
