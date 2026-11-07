@echo off
rem Build MouseBattery.exe. Requires a MinGW-w64 / MSYS2 (or Dev-C++) gcc on PATH,
rem or set the GCC environment variable to a full path before running, e.g.
rem   set GCC=C:\msys64\ucrt64\bin\gcc.exe
setlocal
if "%GCC%"=="" set "GCC=gcc"

rem Make sure gcc's own DLLs (and cc1) are reachable even when GCC is a full path.
for %%I in ("%GCC%") do set "GCCDIR=%%~dpI"
if not "%GCCDIR%"=="" set "PATH=%GCCDIR%;%PATH%"

cd /d "%~dp0"
if not exist dist mkdir dist

"%GCC%" -O2 -s -mwindows -std=gnu99 -DUNICODE -D_UNICODE -finput-charset=UTF-8 ^
  -o dist\MouseBattery.exe src\main.c ^
  -lsetupapi -lhid -lshell32 -lgdi32 -luser32 -ladvapi32
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
echo BUILD OK: dist\MouseBattery.exe
