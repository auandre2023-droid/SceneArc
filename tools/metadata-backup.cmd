@echo off
rem Video Player metadata backup (offline; no network, nothing in the data folder is changed).
rem Copies anilist.json, prefs.json, progress.json, lan_users.json, the lan_progress folder and the app's config.json
rem into a new timestamped folder with a SHA-256 manifest (certutil). Double-click to back up this checkout's player.
rem Usage: metadata-backup.cmd [data-folder] [config.json]
rem   defaults: <this checkout>\host\build-mpv\data and <this checkout>\config.json
rem   destination: %USERPROFILE%\Documents\VideoPlayer-metadata-backups\<yyyymmdd-hhmmss>
rem   (hooks: VP_BACKUP_ROOT = other destination root, VP_BACKUP_LABEL = folder name prefix, VP_NO_PAUSE = 1,
rem    VP_BACKUP_DEST_OUT = file that receives the backup folder path; used by metadata-restore.cmd and the tests)
rem Exit code: 0 = backup complete and verified, 1 = failed.
setlocal EnableExtensions EnableDelayedExpansion
for /f "tokens=2 delims=:." %%C in ('chcp') do set "OLDCP=%%C"
chcp 65001 >nul
set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "DATA=%~1"
if "%DATA%"=="" set "DATA=%ROOT%\host\build-mpv\data"
for %%I in ("%DATA%") do set "DATA=%%~fI"
set "CONFIG=%~2"
if "%CONFIG%"=="" set "CONFIG=%ROOT%\config.json"
for %%I in ("%CONFIG%") do set "CONFIG=%%~fI"
set "BROOT=%VP_BACKUP_ROOT%"
if "%BROOT%"=="" set "BROOT=%USERPROFILE%\Documents\VideoPlayer-metadata-backups"
set "RC=1"

echo ============================================================
echo  影片播放器 元資料備份
echo ============================================================
echo  資料夾：%DATA%
echo  設定檔：%CONFIG%
if not exist "%DATA%\" (
  echo [錯誤] 找不到資料夾：%DATA%
  goto :end
)
if not exist "%DATA%\anilist.json" echo [注意] 資料夾裡沒有 anilist.json（比對資料），其他檔案仍會備份。
tasklist /FI "IMAGENAME eq VideoPlayer.exe" /NH 2>nul | find /I "VideoPlayer.exe" >nul && echo [注意] 播放器正在執行。備份仍會進行；若想取得最一致的備份，請先關閉播放器再執行一次。

rem ---- timestamped destination: the digits of the date and of the time (yyyymmdd-hhmmss on a y/m/d system;
rem      no wmic or PowerShell needed, and no weekday names in the folder name)
call :digits DD "%date%"
call :digits TT "%time%"
if "!TT:~7,1!"=="" set "TT=0!TT!"
set "TS=!DD!-!TT:~0,6!"
set "NAME=%VP_BACKUP_LABEL%%TS%"
set "DEST=%BROOT%\%NAME%"
if exist "%DEST%" set "DEST=%DEST%-%RANDOM%"
mkdir "%DEST%" 2>nul
if not exist "%DEST%\" (
  echo [錯誤] 無法建立備份資料夾：%DEST%
  goto :end
)
set "MAN=%DEST%\manifest-sha256.txt"
type nul > "%MAN%"
set /a COUNT=0, FAIL=0, MOVED=0

for %%F in (anilist.json prefs.json progress.json lan_users.json) do (
  if exist "%DATA%\%%F" (call :one "%DATA%\%%F" "%%F") else echo   略過（不存在）：%%F
)
if exist "%DATA%\lan_progress\" (
  for /r "%DATA%\lan_progress" %%F in (*) do (
    set "P=%%F"
    set "REL=!P:%DATA%\=!"
    call :one "%%F" "!REL!"
  )
) else echo   略過（不存在）：lan_progress\
if exist "%CONFIG%" (call :one "%CONFIG%" "app-root\config.json") else echo   略過（不存在）：config.json

(
  echo source-data=%DATA%
  echo source-config=%CONFIG%
  echo created=%TS%
  echo files=!COUNT!
) > "%DEST%\backup-info.txt"

echo.
if !FAIL! gtr 0 (
  echo [失敗] 有 !FAIL! 個檔案沒有備份成功。備份資料夾：%DEST%
  goto :end
)
if !COUNT! equ 0 (
  echo [失敗] 沒有任何檔案可以備份。
  goto :end
)
echo [完成] 已備份 !COUNT! 個檔案，雜湊都已核對。
if !MOVED! gtr 0 echo [注意] 有 !MOVED! 個檔案在複製時有變動（播放器可能正在寫入）；備份內容與清單一致，但建議關閉播放器後再備份一次。
echo   備份位置：%DEST%
echo   雜湊清單：%MAN%
if defined VP_BACKUP_DEST_OUT (>"%VP_BACKUP_DEST_OUT%" echo %DEST%)
set "RC=0"
goto :end

rem ---- :one <source file> <relative name>: copy, hash both, append to the manifest
:one
set "SRCF=%~1"
set "RELF=%~2"
for %%D in ("%DEST%\%RELF%") do if not exist "%%~dpD" mkdir "%%~dpD" 2>nul
call :hash "%SRCF%"
set "H1=!HASH!"
copy /Y "%SRCF%" "%DEST%\%RELF%" >nul 2>&1
if errorlevel 1 (
  echo   [錯誤] 無法複製：%RELF%
  set /a FAIL+=1
  goto :eof
)
call :hash "%DEST%\%RELF%"
if "!HASH!"=="" (
  echo   [錯誤] 無法計算雜湊：%RELF%
  set /a FAIL+=1
  goto :eof
)
if /I not "!H1!"=="!HASH!" set /a MOVED+=1
>>"%MAN%" echo !HASH!  %RELF%
set /a COUNT+=1
echo   已備份：%RELF%
goto :eof

rem ---- :digits <variable> <text>: keeps only 0-9
:digits
set "_in=%~2"
set "_out="
:digits_next
if not defined _in goto digits_done
set "_c=!_in:~0,1!"
set "_in=!_in:~1!"
set "_d=1"
for /f "delims=0123456789" %%x in ("!_c!") do set "_d="
if defined _d set "_out=!_out!!_c!"
goto digits_next
:digits_done
set "%~1=!_out!"
goto :eof

rem ---- :hash <file>: HASH = SHA-256 hex from certutil (empty on error)
:hash
set "HASH="
for /f "skip=1 delims=" %%H in ('certutil -hashfile "%~1" SHA256 2^>nul') do if not defined HASH set "HASH=%%H"
if defined HASH set "HASH=!HASH: =!"
if defined HASH if not "!HASH:~64!"=="" set "HASH="
if defined HASH if "!HASH:~63,1!"=="" set "HASH="
goto :eof

:end
echo.
if not defined VP_NO_PAUSE pause
chcp %OLDCP% >nul
endlocal & exit /b %RC%
