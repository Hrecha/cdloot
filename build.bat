@echo off
rem ============================================================================
rem  CDLoot build script
rem
rem    build.bat        build core only, deploy to game (hot reload picks it up)
rem    build.bat all    also rebuild the loader - GAME MUST BE CLOSED for that
rem ============================================================================

setlocal
set VS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
set BIN=C:\Steam\steamapps\common\Crimson Desert\bin64
set HERE=%~dp0

call "%VS%" -vcvars_ver=14.36 >nul 2>&1
if errorlevel 1 ( echo ERROR: Visual Studio not found & exit /b 1 )

cd /d "%HERE%"
if not exist build mkdir build

if "%1"=="all" (
  echo [1/2] loader CDLoot.asi
  cl /nologo /O2 /W3 /MT /EHsc /LD src\loader.cpp /Fo:build\ /Fe:CDLoot.asi /link /DLL user32.lib >nul
  if errorlevel 1 ( echo ERROR building loader & exit /b 1 )

  rem The game holds CDLoot.asi open while running - the copy WILL fail then.
  rem Say so loudly instead of pretending it worked.
  copy /y CDLoot.asi "%BIN%\" >nul 2>&1
  if errorlevel 1 (
    echo.
    echo   ***  LOADER NOT DEPLOYED  ***
    echo   The file is locked - close the game first, then run: build.bat all
    echo.
    exit /b 1
  )
  echo       loader deployed - restart the game for it to take effect
)

rem  build.bat plugin  - single self-contained CDLoot.asi, no hot reload.
rem  This is the version to hand to players: one file, nothing to load at
rem  runtime. The game must be CLOSED - it holds the .asi open while running.
if "%1"=="plugin" (
  echo [plugin] CDLoot.asi ^(standalone, no hot reload^)
  cl /nologo /O2 /W3 /MT /EHsc /LD /DCDLOOT_PLUGIN src\core.cpp /Fo:build\ /Fe:CDLoot.asi /link /DLL user32.lib gdi32.lib >nul
  if errorlevel 1 ( echo ERROR building plugin & exit /b 1 )
  rem  The plugin is a DELIVERABLE, not a dev build: it goes to release\ and
  rem  is NOT installed. Installing it would replace the dev loader and delete
  rem  the hot-reload core - which is exactly what happened once by accident.
  rem  To try it in the game, copy release\ into bin64 by hand.
  if not exist release mkdir release
  copy /y CDLoot.asi release\ >nul
  copy /y CDLoot.ini release\ >nul
  echo       built to release\ - NOT installed into the game
  echo       to try it: close the game, copy release\*.* into bin64
  endlocal
  exit /b 0
)

echo [core] CDLoot_core.dll
cl /nologo /O2 /W3 /MT /EHsc /LD src\core.cpp /Fo:build\ /Fe:CDLoot_core.dll /link /DLL user32.lib gdi32.lib >nul
if errorlevel 1 ( echo ERROR building core & exit /b 1 )

copy /y CDLoot_core.dll "%BIN%\" >nul 2>&1
if errorlevel 1 ( echo ERROR: cannot copy core to game folder & exit /b 1 )

echo       core deployed - the running game reloads it within a second
endlocal
