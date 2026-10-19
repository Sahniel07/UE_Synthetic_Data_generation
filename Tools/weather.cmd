@echo off
setlocal enabledelayedexpansion
rem ==================================================================================================
rem  weather.cmd - run weather_pipeline.py with a real Python, from any directory.
rem
rem      .\Tools\weather.cmd                 capture every city in weather_pipeline.py
rem      .\Tools\weather.cmd --city Sydney   one city
rem      .\Tools\weather.cmd --imagecount    image count and time of a run, run nothing
rem      .\Tools\weather.cmd --test          run the tests (no Unreal needed)
rem
rem  Every option is weather_pipeline.py's own; this only finds a real Python for it. `python` on
rem  Windows is often the Microsoft Store placeholder, so Unreal's bundled Python is used first.
rem  Override with: set UE_PYTHON=C:\path\python.exe
rem ==================================================================================================

set "PYTHON="
if defined UE_PYTHON if exist "%UE_PYTHON%" set "PYTHON=%UE_PYTHON%"

if not defined PYTHON (
    for %%R in ("%UE_ENGINE_DIR%" "C:\Program Files\Epic Games\UE_5.5" "C:\Program Files\Epic Games\UE_5.4") do (
        if not defined PYTHON if exist "%%~R\Engine\Binaries\ThirdParty\Python3\Win64\python.exe" (
            set "PYTHON=%%~R\Engine\Binaries\ThirdParty\Python3\Win64\python.exe"
        )
    )
)

if not defined PYTHON (
    for /f "delims=" %%P in ('where python.exe 2^>nul') do (
        if not defined PYTHON (
            echo %%P | find /i "WindowsApps" >nul
            if errorlevel 1 set "PYTHON=%%P"
        )
    )
)

if not defined PYTHON (
    echo No usable Python found. Install Unreal Engine 5.5 or Python, or set UE_PYTHON.
    exit /b 1
)

pushd "%~dp0"
"%PYTHON%" weather_pipeline.py %*
set "CODE=%ERRORLEVEL%"
popd
exit /b %CODE%
