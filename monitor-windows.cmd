@echo off
setlocal
rem Native CMD/PowerShell entry point. No SDK activation, build or flashing.
if defined IDF_PYTHON_ENV_PATH if exist "%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" (
  "%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" -X utf8 "%~dp0firmware\scripts\monitor.py" %*
  exit /b
)
where python >nul 2>nul
if not errorlevel 1 (
  python -X utf8 "%~dp0firmware\scripts\monitor.py" %*
  exit /b
)
where py >nul 2>nul
if not errorlevel 1 (
  py -3 -X utf8 "%~dp0firmware\scripts\monitor.py" %*
  exit /b
)
echo Python 3 is required. Install Python with pyserial, or use an ESP-IDF terminal. 1>&2
exit /b 1
