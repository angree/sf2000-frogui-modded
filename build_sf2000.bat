@echo off
REM ========================================
REM  FrogUI v79 Build Script for SF2000
REM ========================================
REM
REM EDIT THIS PATH to point to your sf2000_multicore directory:
REM
set "MULTICORE_PATH=C:\Temp_FrogUI\sf2000_multicore"
REM
REM ========================================

echo ========================================
echo  FrogUI v79 - SF2000 Build
echo ========================================
echo.
echo Multicore path: %MULTICORE_PATH%
echo.

REM Check if multicore directory exists
if not exist "%MULTICORE_PATH%\Makefile" (
    echo ERROR: sf2000_multicore not found at %MULTICORE_PATH%
    echo Please edit MULTICORE_PATH in this batch file.
    pause
    exit /b 1
)

REM Copy FrogUI sources to multicore
echo Copying FrogUI sources to multicore...
xcopy /E /Y /Q "%~dp0cores\menu\*" "%MULTICORE_PATH%\cores\menu\" >nul

REM Convert multicore path to WSL path
set "WSL_PATH=%MULTICORE_PATH:\=/%"
set "WSL_PATH=%WSL_PATH:C:=/mnt/c%"
set "WSL_PATH=%WSL_PATH:D:=/mnt/d%"

echo Building from: %WSL_PATH%
echo.

wsl bash -c "cd '%WSL_PATH%' && make clean CORE=cores/menu FROGGY_TYPE=SF2000 && make CORE=cores/menu FROGGY_TYPE=SF2000"

if %ERRORLEVEL% EQU 0 (
    copy "%MULTICORE_PATH%\core_87000000" "%~dp0core_87000000" >nul
    echo.
    echo ========================================
    echo  BUILD SUCCESSFUL!
    echo  Output: %~dp0core_87000000
    echo ========================================
) else (
    echo.
    echo ========================================
    echo  BUILD FAILED!
    echo ========================================
)

pause
