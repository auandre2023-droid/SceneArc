@echo off
setlocal EnableExtensions
set "EXE=%~dp0host\build-mpv\VideoPlayer.exe"
if not exist "%EXE%" (
  echo MPV edition has not been built. Run build-mpv.cmd first.
  exit /b 1
)
start "" "%EXE%"
