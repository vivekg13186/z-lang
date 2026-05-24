@echo off
REM Build the z interpreter on Windows.
REM
REM This script tries MSVC's `cl` first, then falls back to MinGW's `gcc`.
REM Run from a Developer Command Prompt (for cl) or a normal shell (for gcc).

setlocal

where cl >nul 2>&1
if %errorlevel%==0 (
    echo Building with MSVC...
    cl /nologo /W3 /O2 /D_CRT_SECURE_NO_WARNINGS z.c /Fe:z.exe
    if errorlevel 1 goto :err
    if exist z.obj del z.obj
    echo Built z.exe
    goto :ok
)

where gcc >nul 2>&1
if %errorlevel%==0 (
    echo Building with MinGW gcc...
    gcc -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-unused-result z.c -o z.exe
    if errorlevel 1 goto :err
    echo Built z.exe
    goto :ok
)

echo Could not find cl or gcc on PATH.
echo Install either Visual Studio Build Tools or MinGW-w64, then retry.
exit /b 1

:err
echo Build failed.
exit /b 1

:ok
endlocal
exit /b 0
