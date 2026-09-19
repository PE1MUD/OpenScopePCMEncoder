@echo off
setlocal EnableExtensions EnableDelayedExpansion

cd /d "%~dp0"

rem ============================================================
rem OpenScopePCMEncoder backup
rem
rem Output example:
rem   20260905_134512V0.1.0.zip
rem
rem Version is read from CMakeLists.txt:
rem   project(OpenScopePCMEncoder VERSION 0.1.0 LANGUAGES CXX)
rem ============================================================

rem ------------------------------------------------------------
rem Timestamp
rem ------------------------------------------------------------

for /f %%I in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do (
    set "STAMP=%%I"
)

rem ------------------------------------------------------------
rem Read VERSION from CMakeLists.txt
rem ------------------------------------------------------------

set "VERSION="

if exist "CMakeLists.txt" (
    for /f "usebackq delims=" %%I in (`
        powershell -NoProfile -Command ^
        "$t = Get-Content -Raw 'CMakeLists.txt';" ^
        "$m = [regex]::Match($t, '(?is)project\s*\([^)]*?\bVERSION\s+([0-9]+(?:\.[0-9]+){1,3})');" ^
        "if ($m.Success) { $m.Groups[1].Value }"
    `) do (
        set "VERSION=%%I"
    )
)

rem ------------------------------------------------------------
rem Fallback if no version could be found
rem ------------------------------------------------------------

if not defined VERSION (
    echo.
    echo WARNING: No VERSION found in CMakeLists.txt.
    echo Using Vunknown.
    echo.
    set "VERSION=unknown"
)

rem ------------------------------------------------------------
rem File names
rem ------------------------------------------------------------

set "ZIPNAME=%STAMP%V%VERSION%.zip"
set "TEMPDIR=%TEMP%\OpenScopePCMEncoder_backup_%STAMP%"

echo.
echo ============================================================
echo   OpenScopePCMEncoder Backup
echo ============================================================
echo.
echo Version : V%VERSION%
echo Backup  : %ZIPNAME%
echo.

rem ------------------------------------------------------------
rem Prepare temporary directory
rem ------------------------------------------------------------

if exist "%TEMPDIR%" (
    rmdir /s /q "%TEMPDIR%"
)

mkdir "%TEMPDIR%\OpenScopePCMEncoder"

if errorlevel 1 (
    echo ERROR: Could not create temporary directory.
    pause
    exit /b 1
)

rem ------------------------------------------------------------
rem Copy source tree
rem ------------------------------------------------------------

robocopy "%CD%" "%TEMPDIR%\OpenScopePCMEncoder" /E ^
    /XD ^
        ".git" ^
        ".vs" ^
        ".idea" ^
        "build" ^
        "build-debug" ^
        "build-release" ^
        "build-debug-vs" ^
        "build-release-vs" ^
        "out" ^
        "bin" ^
        "obj" ^
        "Debug" ^
        "Release" ^
        "RelWithDebInfo" ^
        "MinSizeRel" ^
        "x64" ^
        "x86" ^
        "CMakeFiles" ^
        "vcpkg_installed" ^
    /XF ^
        "*.zip" ^
        "*.7z" ^
        "*.rar" ^
        "*.user" ^
        "*.suo" ^
        "*.VC.db" ^
        "*.VC.opendb" ^
        "*.obj" ^
        "*.o" ^
        "*.pch" ^
        "*.pdb" ^
        "*.ilk" ^
        "*.idb" ^
        "*.exe" ^
        "*.dll" ^
        "*.lib" ^
        "*.exp" ^
        "*.log" ^
        "*.tmp" ^
        "*.bak" ^
        "*~" ^
    /NFL /NDL /NJH /NJS /NP

set "RC=%ERRORLEVEL%"

if %RC% GEQ 8 (
    echo.
    echo ERROR: Robocopy failed with error %RC%.
    rmdir /s /q "%TEMPDIR%"
    pause
    exit /b 1
)

rem ------------------------------------------------------------
rem Create ZIP
rem ------------------------------------------------------------

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "Compress-Archive -Path '%TEMPDIR%\OpenScopePCMEncoder' -DestinationPath '%CD%\%ZIPNAME%' -CompressionLevel Optimal -Force"

if errorlevel 1 (
    echo.
    echo ERROR: ZIP creation failed.
    rmdir /s /q "%TEMPDIR%"
    pause
    exit /b 1
)

rem ------------------------------------------------------------
rem Cleanup
rem ------------------------------------------------------------

rmdir /s /q "%TEMPDIR%"

echo.
echo ============================================================
echo   Backup complete
echo ============================================================
echo.
echo %CD%\%ZIPNAME%
echo.

pause
endlocal
