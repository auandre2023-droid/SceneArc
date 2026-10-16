@echo off
rem Video Player: rebuild the library-scoped bangumi-data index (ui\vendor\bangumi-data\index.json). Offline: no network, no download,
rem nothing in the data folder is changed (the store is copied to a temporary folder first and only the copy is read).
rem Run it after new works were added to the library and matched (works added later get dataset suggestions only once their ids are
rem in the index), then restart the player. Double-click to use the defaults.
rem Usage: bangumi-data-index.cmd [data.json] [anilist.json]
rem   defaults: data.json = %LOCALAPPDATA%\Temp\jfr\bangumi-data\v0.3.231\data.json (the pinned bangumi-data v0.3.231 dist/data.json,
rem             SHA-256 da3ec080c41a2015d658572a70d140556a51fa66e3dde3e18894dcee1974111b; any other file is refused)
rem             anilist.json = <this checkout>\host\build-mpv\data\anilist.json (copied, never written)
rem   (hooks: VP_NO_PAUSE = 1, VP_INDEX_OUT = another output file)
rem Exit code: 0 = index written, 1 = failed / refused (nothing written).
rem This file is ASCII on purpose: cmd.exe misreads lines of a UTF-8 batch file after chcp 65001 (a jump to a label can run part of a line);
rem the Traditional Chinese messages come from the generator (--zh).
setlocal EnableExtensions
for /f "tokens=2 delims=:." %%C in ('chcp') do set "OLDCP=%%C"
chcp 65001 >nul
set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "DATASET=%~1"
if "%DATASET%"=="" set "DATASET=%LOCALAPPDATA%\Temp\jfr\bangumi-data\v0.3.231\data.json"
set "STORE=%~2"
if "%STORE%"=="" set "STORE=%ROOT%\host\build-mpv\data\anilist.json"
set "OUT=%VP_INDEX_OUT%"
if "%OUT%"=="" set "OUT=%ROOT%\ui\vendor\bangumi-data\index.json"
set "RC=1"
set "TMPD=%TEMP%\vp-bgm-index-%RANDOM%%RANDOM%"
where node >nul 2>nul
if errorlevel 1 (
  echo [ERROR] node.exe was not found. Install Node.js from https://nodejs.org and run this again.
  goto :end
)
if not exist "%STORE%" (
  echo [ERROR] The metadata store was not found: %STORE%
  goto :end
)
mkdir "%TMPD%" 2>nul
copy /y "%STORE%" "%TMPD%\anilist.json" >nul
if errorlevel 1 (
  echo [ERROR] The metadata store could not be copied.
  goto :cleanup
)
node "%ROOT%\tools\bangumi-data-index.js" --zh --dataset "%DATASET%" --store "%TMPD%\anilist.json" --out "%OUT%"
if not errorlevel 1 set "RC=0"
:cleanup
if exist "%TMPD%\" rmdir /s /q "%TMPD%"
:end
chcp %OLDCP% >nul
if not defined VP_NO_PAUSE pause
exit /b %RC%
