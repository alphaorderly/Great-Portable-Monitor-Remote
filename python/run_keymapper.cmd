@echo off
setlocal
if not exist "%~dp0..\.venv-gui\Scripts\python.exe" (
  echo Python environment missing. See python/README.md.
  exit /b 1
)
"%~dp0..\.venv-gui\Scripts\python.exe" "%~dp0run_keymapper.py" %*
exit /b %errorlevel%
