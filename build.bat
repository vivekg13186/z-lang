@echo off
REM Build the z interpreter on Windows.
REM
REM This script tries MSVC's `cl` first, then falls back to MinGW's `gcc`.
REM Run from a Developer Command Prompt (for cl) or a normal shell (for gcc).
REM
REM Pass `--image` to enable the optional image module
REM (requires ImageMagick on PATH at runtime).

setlocal

set EXTRA_DEF=
set EXTRA_MSVC=
if /I "%1"=="--image" (
    set EXTRA_DEF=-DZ_WITH_IMAGE
    set EXTRA_MSVC=/DZ_WITH_IMAGE
)

set DIST=dist\windows_x86
if not exist "%DIST%" mkdir "%DIST%"

where cl >nul 2>&1
if %errorlevel%==0 (
    echo Building with MSVC...
    cl /nologo /W3 /O2 /D_CRT_SECURE_NO_WARNINGS %EXTRA_MSVC% z.c /Fe:%DIST%\z.exe /Fo%DIST%\z.obj
    if errorlevel 1 goto :err
    cl /nologo /W3 /O2 /D_CRT_SECURE_NO_WARNINGS %EXTRA_MSVC% zide.c /Fe:%DIST%\zide.exe /Fo%DIST%\zide.obj
    if errorlevel 1 goto :err
    if exist %DIST%\z.obj del %DIST%\z.obj
    if exist %DIST%\zide.obj del %DIST%\zide.obj
    copy /Y %DIST%\z.exe z.exe >nul
    copy /Y %DIST%\zide.exe zide.exe >nul
    echo Built %DIST%\z.exe and %DIST%\zide.exe
    goto :ok
)

where gcc >nul 2>&1
if %errorlevel%==0 (
    echo Building with MinGW gcc...
    gcc -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-unused-result -Wno-unused-function %EXTRA_DEF% z.c -o %DIST%\z.exe
    if errorlevel 1 goto :err
    gcc -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-unused-result -Wno-unused-function %EXTRA_DEF% zide.c -o %DIST%\zide.exe
    if errorlevel 1 goto :err
    copy /Y %DIST%\z.exe z.exe >nul
    copy /Y %DIST%\zide.exe zide.exe >nul
    echo Built %DIST%\z.exe and %DIST%\zide.exe
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
