@echo off
rem Video Player metadata restore (offline). Puts a backup made by metadata-backup.cmd back into the data folder.
rem Refuses while VideoPlayer.exe is running, verifies every file of the backup against its SHA-256 manifest first,
rem backs up the current files (pre-restore-...) before overwriting anything, then verifies the restored files.
rem Usage: metadata-restore.cmd <backup-folder> [data-folder] [config.json]
rem   (or drag the backup folder onto this file; without arguments the existing backups are listed)
rem   defaults: <this checkout>\host\build-mpv\data and <this checkout>\config.json
rem   (test hooks: VP_BACKUP_ROOT, VP_NO_PAUSE = 1, VP_EXE_NAME = the process name that blocks a restore)
rem Exit code: 0 = restored and verified, 1 = refused or failed (2 = backup does not verify; nothing changed).
setlocal EnableExtensions EnableDelayedExpansion
for /f "tokens=2 delims=:." %%C in ('chcp') do set "OLDCP=%%C"
chcp 65001 >nul
set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "BK=%~1"
set "DATA=%~2"
if "%DATA%"=="" set "DATA=%ROOT%\host\build-mpv\data"
set "CONFIG=%~3"
if "%CONFIG%"=="" set "CONFIG=%ROOT%\config.json"
set "BROOT=%VP_BACKUP_ROOT%"
if "%BROOT%"=="" set "BROOT=%USERPROFILE%\Documents\VideoPlayer-metadata-backups"
set "EXE=%VP_EXE_NAME%"
if "%EXE%"=="" set "EXE=VideoPlayer.exe"
set "RC=1"

echo ============================================================
echo  影片播放器 元資料還原
echo ============================================================
if "%BK%"=="" (
  echo 用法：metadata-restore.cmd ^<備份資料夾^>   （也可以把備份資料夾拖到這個檔案上）
  echo.
  echo 目前的備份（%BROOT%）：
  if exist "%BROOT%\" (dir /b /ad /o-n "%BROOT%") else echo   （沒有）
  goto :end
)
for %%I in ("%BK%") do set "BK=%%~fI"
for %%I in ("%DATA%") do set "DATA=%%~fI"
for %%I in ("%CONFIG%") do set "CONFIG=%%~fI"
echo  備份：%BK%
echo  還原到：%DATA%
echo  設定檔：%CONFIG%
echo.

tasklist /FI "IMAGENAME eq %EXE%" /NH 2>nul | find /I "%EXE%" >nul
if not errorlevel 1 (
  echo [拒絕] 播放器（%EXE%）正在執行。請先關閉播放器，再執行還原。什麼都沒有改變。
  goto :end
)
set "MAN=%BK%\manifest-sha256.txt"
if not exist "%MAN%" (
  echo [拒絕] 這個資料夾不是元資料備份（找不到 manifest-sha256.txt）。什麼都沒有改變。
  goto :end
)
if not exist "%DATA%\" (
  echo [拒絕] 找不到要還原的資料夾：%DATA%
  goto :end
)

rem ---- 1. the backup must verify completely before anything is touched
echo 1/4 核對備份檔案的雜湊…
set /a N=0, BAD=0
for /f "usebackq tokens=1,* delims= " %%A in ("%MAN%") do (
  set /a N+=1
  call :hash "%BK%\%%B"
  if /I not "!HASH!"=="%%A" (
    echo   [不符] %%B
    set /a BAD+=1
  )
)
if !N! equ 0 (
  echo [拒絕] 雜湊清單是空的。什麼都沒有改變。
  goto :end
)
if !BAD! gtr 0 (
  echo [拒絕] 備份中有 !BAD! 個檔案與雜湊清單不符（可能已損壞或被修改）。什麼都沒有改變。
  set "RC=2"
  goto :end
)
echo   !N! 個檔案都相符。

rem ---- 2. keep the current files first
echo 2/4 先備份目前的資料（pre-restore）…
set "KEEP_NP=%VP_NO_PAUSE%"
set "VP_BACKUP_LABEL=pre-restore-"
set "VP_NO_PAUSE=1"
set "VP_BACKUP_DEST_OUT=%TEMP%\vp-pre-restore-dest.txt"
del "%TEMP%\vp-pre-restore-dest.txt" 2>nul
call "%~dp0metadata-backup.cmd" "%DATA%" "%CONFIG%" >"%TEMP%\vp-pre-restore.log" 2>&1
set "PRE_RC=!errorlevel!"
set "VP_NO_PAUSE=%KEEP_NP%"
set "VP_BACKUP_LABEL="
set "VP_BACKUP_DEST_OUT="
chcp 65001 >nul
if not "!PRE_RC!"=="0" (
  type "%TEMP%\vp-pre-restore.log"
  echo [拒絕] 無法先備份目前的資料，為了安全不進行還原。什麼都沒有改變。
  goto :end
)
set "PRE="
if exist "%TEMP%\vp-pre-restore-dest.txt" set /p PRE=<"%TEMP%\vp-pre-restore-dest.txt"
echo   目前的資料已備份到：!PRE!

rem ---- 3. copy back
echo 3/4 還原檔案…
set /a DONE=0, FAIL=0
for /f "usebackq tokens=1,* delims= " %%A in ("%MAN%") do (
  set "REL=%%B"
  if /I "!REL!"=="app-root\config.json" (set "TO=%CONFIG%") else set "TO=%DATA%\!REL!"
  call :put "%BK%\%%B" "!TO!" "%%A" "%%B"
)

rem ---- 4. result
echo 4/4 結果
if !FAIL! gtr 0 (
  echo [失敗] !FAIL! 個檔案沒有還原成功（其餘 !DONE! 個已還原並核對）。還原前的資料在上面的 pre-restore 資料夾。
  goto :end
)
echo [完成] 已還原 !DONE! 個檔案，雜湊全部相符。現在可以開啟播放器。
set "RC=0"
goto :end

rem ---- :put <from> <to> <expected hash> <name>
:put
for %%D in ("%~2") do if not exist "%%~dpD" mkdir "%%~dpD" 2>nul
copy /Y "%~1" "%~2" >nul 2>&1
if errorlevel 1 (
  echo   [錯誤] 無法寫入：%~4
  set /a FAIL+=1
  goto :eof
)
call :hash "%~2"
if /I not "!HASH!"=="%~3" (
  echo   [錯誤] 寫入後雜湊不符：%~4
  set /a FAIL+=1
  goto :eof
)
set /a DONE+=1
echo   已還原：%~4
goto :eof

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
