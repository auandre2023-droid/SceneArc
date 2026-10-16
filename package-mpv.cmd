@echo off
rem Builds the MPV edition and wraps it into one installer exe:
rem   dist\SceneArc-Setup-<VERSION.txt>.exe, or -<commit date>-<commit>.exe without VERSION.txt
rem Usage: package-mpv.cmd [--skip-build]
rem   --skip-build  package the existing host\build-mpv\VideoPlayer.exe as is
rem Needs Inno Setup 6 (winget install JRSoftware.InnoSetup).
setlocal EnableExtensions

set "ROOT=%~dp0"
set "HOST_BUILD=%ROOT%host\build-mpv"
set "DIST=%ROOT%dist"
set "STAGE=%DIST%\stage-mpv"
set "LOGS=%ROOT%build-logs"
set "PKG_LOG=%LOGS%\mpv-package.log"
set "PKG_FAILED="

if not exist "%LOGS%" mkdir "%LOGS%"

if /i "%~1"=="--skip-build" goto :tools
echo == build
call "%ROOT%build-mpv.cmd"
if errorlevel 1 (
  echo MPV build failed; nothing was packaged.
  set "PKG_FAILED=1"
  goto :finish
)

:tools
echo == tools
set "ISCC="
for %%P in ("%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" "%ProgramFiles%\Inno Setup 6\ISCC.exe") do (
  if not defined ISCC if exist "%%~P" set "ISCC=%%~P"
)
if not defined ISCC for /f "delims=" %%I in ('where ISCC.exe 2^>nul') do if not defined ISCC set "ISCC=%%I"
if not defined ISCC (
  echo Inno Setup 6 was not found. Install it with: winget install JRSoftware.InnoSetup
  set "PKG_FAILED=1"
  goto :finish
)

rem The VC++ redistributable comes from the same Visual Studio that build-mpv.cmd compiles with.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS="
if exist "%VSWHERE%" for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%I"
set "REDIST_VER="
if defined VS if exist "%VS%\VC\Auxiliary\Build\Microsoft.VCRedistVersion.default.txt" (
  set /p REDIST_VER=<"%VS%\VC\Auxiliary\Build\Microsoft.VCRedistVersion.default.txt"
)
if not defined REDIST_VER (
  echo Visual Studio VC++ redistributable version file not found.
  set "PKG_FAILED=1"
  goto :finish
)
set "VC_REDIST=%VS%\VC\Redist\MSVC\%REDIST_VER%\vc_redist.x64.exe"
if not exist "%VC_REDIST%" (
  echo vc_redist.x64.exe not found: "%VC_REDIST%"
  set "PKG_FAILED=1"
  goto :finish
)
for /f "tokens=1,2 delims=." %%A in ("%REDIST_VER%") do (
  set "CRT_MAJOR=%%A"
  set "CRT_MINOR=%%B"
)

rem Version = date of the packaged commit; the commit hash goes into the file name.
set "COMMIT=unknown"
set "VERSION=0.0.0"
for /f "delims=" %%I in ('git -C "%ROOT%." rev-parse --short HEAD 2^>nul') do set "COMMIT=%%I"
for /f "tokens=2" %%I in ('git -C "%ROOT%." log -1 --date^=short 2^>nul ^| findstr /b /c:"Date:"') do set "COMMIT_DATE=%%I"
if defined COMMIT_DATE for /f "tokens=1-3 delims=-" %%A in ("%COMMIT_DATE%") do (
  set "VER_Y=%%A"
  set "VER_M=%%B"
  set "VER_D=%%C"
)
if defined VER_Y (
  set /a VER_M=1%VER_M%-100
  set /a VER_D=1%VER_D%-100
)
if defined VER_Y set "VERSION=%VER_Y%.%VER_M%.%VER_D%"
git -C "%ROOT%." diff --quiet HEAD 2>nul
if errorlevel 1 set "COMMIT=%COMMIT%-dirty"
set "OUT_NAME=SceneArc-Setup-%VERSION%-%COMMIT%"
rem A release version in VERSION.txt replaces the date version and drops the commit from the file name.
if exist "%ROOT%VERSION.txt" set /p RELEASE_VER=<"%ROOT%VERSION.txt"
if defined RELEASE_VER (
  set "VERSION=%RELEASE_VER%"
  set "OUT_NAME=SceneArc-Setup-%RELEASE_VER%"
)
rem The numeric file version drops a pre-release suffix (0.1.0-beta gives 0.1.0).
for /f "tokens=1 delims=-" %%V in ("%VERSION%") do set "VERSION_NUM=%%V"
rem The About page shows APP_VERSION from ui\app.js; it must match the version being packaged.
if defined RELEASE_VER findstr /l /c:"const APP_VERSION = '%RELEASE_VER%';" "%ROOT%ui\app.js" >nul || (
  echo ui\app.js APP_VERSION does not match VERSION.txt ^(%RELEASE_VER%^); nothing was packaged.
  set "PKG_FAILED=1"
  goto :finish
)
echo Packaging %VERSION% (%COMMIT%), VC++ runtime %REDIST_VER%

echo == stage
for %%F in ("%HOST_BUILD%\VideoPlayer.exe" "%HOST_BUILD%\libmpv-2.dll" "%ROOT%ui\index.html" "%ROOT%mpv\catalog.json" "%ROOT%vs-runtime\VSScript.dll" "%ROOT%vs-runtime\portable.vs" "%ROOT%LICENSE" "%ROOT%THIRD_PARTY_NOTICES.md" "%ROOT%licenses\README.md") do (
  if not exist "%%~F" (
    echo Missing %%~F
    set "PKG_FAILED=1"
  )
)
if defined PKG_FAILED (
  echo The build output or runtime assets are incomplete; nothing was packaged.
  goto :finish
)
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
> "%PKG_LOG%" echo SceneArc package %VERSION% (%COMMIT%) %DATE% %TIME%
rem Exe, its symbols (crash dumps) and libmpv + FFmpeg DLLs. config.json and data\ are never staged.
copy /y "%HOST_BUILD%\VideoPlayer.exe" "%STAGE%\" >> "%PKG_LOG%" || set "PKG_FAILED=1"
copy /y "%HOST_BUILD%\VideoPlayer.pdb" "%STAGE%\" >> "%PKG_LOG%" || set "PKG_FAILED=1"
copy /y "%HOST_BUILD%\*.dll" "%STAGE%\" >> "%PKG_LOG%" || set "PKG_FAILED=1"
robocopy "%ROOT%ui" "%STAGE%\ui" /E /NFL /NDL /NJH /NP >> "%PKG_LOG%"
if errorlevel 8 set "PKG_FAILED=1"
robocopy "%ROOT%mpv" "%STAGE%\mpv" /E /NFL /NDL /NJH /NP >> "%PKG_LOG%"
if errorlevel 8 set "PKG_FAILED=1"
robocopy "%ROOT%vs-runtime" "%STAGE%\vs-runtime" /E /NFL /NDL /NJH /NP >> "%PKG_LOG%"
if errorlevel 8 set "PKG_FAILED=1"
rem The program's GPL-3.0 license, the third-party list and the full license texts of the bundled components.
copy /y "%ROOT%LICENSE" "%STAGE%\LICENSE.txt" >> "%PKG_LOG%" || set "PKG_FAILED=1"
copy /y "%ROOT%THIRD_PARTY_NOTICES.md" "%STAGE%\" >> "%PKG_LOG%" || set "PKG_FAILED=1"
robocopy "%ROOT%licenses" "%STAGE%\licenses" /E /NFL /NDL /NJH /NP >> "%PKG_LOG%"
if errorlevel 8 set "PKG_FAILED=1"
if defined PKG_FAILED (
  echo Staging failed; see "%PKG_LOG%"
  goto :finish
)

echo == installer (compressing about 800 MB, this takes a few minutes)
"%ISCC%" /Q "/DAppVersion=%VERSION%" "/DAppVersionNum=%VERSION_NUM%" "/DAppCommit=%COMMIT%" "/DStageDir=%STAGE%" "/DVCRedist=%VC_REDIST%" "/DOutputDir=%DIST%" "/DOutputName=%OUT_NAME%" "/DMinCrtMajor=%CRT_MAJOR%" "/DMinCrtMinor=%CRT_MINOR%" "%ROOT%installer\VideoPlayerMPV.iss" >> "%PKG_LOG%" 2>&1
if errorlevel 1 (
  type "%PKG_LOG%"
  echo Inno Setup failed; see "%PKG_LOG%"
  set "PKG_FAILED=1"
  goto :finish
)
set "SETUP_EXE=%DIST%\%OUT_NAME%.exe"
>> "%PKG_LOG%" echo output %SETUP_EXE%
echo Installer: %SETUP_EXE%

:finish
rem Keep the window open when the script was double-clicked.
echo %CMDCMDLINE% | find /i "%~nx0" >nul && pause
if defined PKG_FAILED exit /b 1
exit /b 0
