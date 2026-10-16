@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
set "LOGS=%ROOT%build-logs"
set "SUMMARY=%LOGS%\mpv-summary.txt"
set "HOST_BUILD=%ROOT%host\build-mpv"
set "LIBMPV=%ROOT%deps\mpv-sdk"
set "FFMPEG=%ROOT%deps\ffmpeg-sdk\ffmpeg-n8.1.3-17-gc389840894-win64-lgpl-shared-8.1"
set "WEBVIEW2=%ROOT%host\third_party\webview2"
set "FAILED="

if not exist "%LOGS%" mkdir "%LOGS%"
> "%SUMMARY%" echo Video Player MPV build %DATE% %TIME%

if not exist "%WEBVIEW2%\build\native\include\WebView2.h" (
  >> "%SUMMARY%" echo webview2-sdk missing
  echo WebView2 SDK is not present at %WEBVIEW2%.
  exit /b 1
)
>> "%SUMMARY%" echo webview2-sdk cached

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  >> "%SUMMARY%" echo visual-studio missing
  echo Visual Studio locator not found.
  exit /b 1
)
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%I"
if not defined VS (
  >> "%SUMMARY%" echo visual-studio missing
  echo Visual Studio C++ tools not found.
  exit /b 1
)
call "%VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 (
  >> "%SUMMARY%" echo visual-studio-env failed
  echo Visual Studio developer environment failed.
  exit /b 1
)

set "CMAKE=%ProgramFiles%\CMake\bin\cmake.exe"
if not exist "%CMAKE%" set "CMAKE=cmake"
where cmake >nul 2>nul
if errorlevel 1 (
  >> "%SUMMARY%" echo cmake missing
  echo CMake not found.
  exit /b 1
)
set "NINJA=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not exist "%NINJA%" (
  >> "%SUMMARY%" echo ninja missing
  echo Visual Studio Ninja not found.
  exit /b 1
)
if not exist "%LIBMPV%\include\mpv\client.h" (
  >> "%SUMMARY%" echo libmpv-sdk missing
  echo Pinned libmpv SDK is incomplete.
  exit /b 1
)
if not exist "%FFMPEG%\lib\avcodec.lib" (
  >> "%SUMMARY%" echo ffmpeg-sdk missing
  echo BtbN FFmpeg LGPL shared development package is incomplete.
  exit /b 1
)

echo == mpv-host-configure
"%CMAKE%" -S "%ROOT%host" -B "%HOST_BUILD%" -G Ninja "-DCMAKE_MAKE_PROGRAM=%NINJA%" "-DCMAKE_BUILD_TYPE=Release" "-DVP_ENGINE=mpv" "-DLIBMPV_DIR=%LIBMPV%" "-DFFMPEG_DIR=%FFMPEG%" "-DWEBVIEW2_DIR=%WEBVIEW2%" > "%LOGS%\mpv-host-configure.log" 2>&1
set "RC=%ERRORLEVEL%"
type "%LOGS%\mpv-host-configure.log"
>> "%SUMMARY%" echo mpv-host-configure exit=%RC%
if not "%RC%"=="0" set "FAILED=1"
if not "%RC%"=="0" goto :finish

echo == mpv-host-build
"%CMAKE%" --build "%HOST_BUILD%" --config Release > "%LOGS%\mpv-host-build.log" 2>&1
set "RC=%ERRORLEVEL%"
findstr /i /c:"error C" /c:"fatal error" /c:"FAILED:" /c:"ninja: build stopped" "%LOGS%\mpv-host-build.log" > "%LOGS%\mpv-build-diagnostics.log"
type "%LOGS%\mpv-build-diagnostics.log"
call :record mpv-host-build %RC%
if not "%RC%"=="0" goto :finish

echo == mpv-runtime-deploy
set "RC=0"
> "%LOGS%\mpv-runtime-deploy.log" echo Runtime asset deployment
if exist "%ROOT%mpv" (
  if not exist "%HOST_BUILD%\mpv" mkdir "%HOST_BUILD%\mpv"
  robocopy "%ROOT%mpv" "%HOST_BUILD%\mpv" /E /NFL /NDL /NJH /NJS /NP > "%LOGS%\mpv-runtime-deploy.log"
  if errorlevel 8 set "RC=1"
)
if exist "%ROOT%vs-runtime" (
  if not exist "%HOST_BUILD%\vs-runtime" mkdir "%HOST_BUILD%\vs-runtime"
  robocopy "%ROOT%vs-runtime" "%HOST_BUILD%\vs-runtime" /E /NFL /NDL /NJH /NJS /NP >> "%LOGS%\mpv-runtime-deploy.log"
  if errorlevel 8 set "RC=1"
)
type "%LOGS%\mpv-runtime-deploy.log"
call :record mpv-runtime-deploy %RC%

:finish
type "%SUMMARY%"
if defined FAILED exit /b 1
echo MPV edition built at host\build-mpv\VideoPlayer.exe.
exit /b 0

:record
set "STEP=%~1"
set "RC=%~2"
>> "%SUMMARY%" echo %STEP% exit=%RC%
if not "%RC%"=="0" set "FAILED=1"
exit /b 0
