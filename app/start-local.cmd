@echo off
setlocal
cd /d "%~dp0"

set "PORT=8000"
set "URL=http://localhost:%PORT%/rc-car-controller.html"

echo ============================================================
echo   RC Car Remote  -  local server
echo ============================================================
echo.
echo   Folder : %CD%
echo   Address: %URL%
echo.
echo   Leave this window OPEN while you drive.
echo   Press Ctrl+C (or close the window) to stop.
echo.

rem ---- find a Python interpreter -------------------------------------------
set "PY="
where py >nul 2>nul && set "PY=py"
if not defined PY ( where python >nul 2>nul && set "PY=python" )
if not defined PY if exist "%LOCALAPPDATA%\Programs\Python\Python312\python.exe" set "PY=%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
if not defined PY if exist "%USERPROFILE%\.workbuddy-ai\binaries\python\versions\3.13.12\python.exe" set "PY=%USERPROFILE%\.workbuddy-ai\binaries\python\versions\3.13.12\python.exe"

if not defined PY (
  echo   Python was not found on this PC.
  echo.
  echo   That is fine - you do not need it. Just double-click
  echo   rc-car-controller.html to open the app directly.
  echo.
  pause
  exit /b 1
)

echo   Using Python: %PY%
echo.

start "" "%URL%"
%PY% -m http.server %PORT% --bind 127.0.0.1

endlocal
