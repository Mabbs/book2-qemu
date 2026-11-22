@echo off
setlocal
rem Use Windows PowerShell directly; Python and PATH setup are not required.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0start.ps1" %* 2>"%TEMP%\book2-start-errors.log"
set "BOOK2_EXIT=%ERRORLEVEL%"
if not "%BOOK2_EXIT%"=="0" (
  echo.
  type "%TEMP%\book2-start-errors.log"
  echo Book2 exited with error %BOOK2_EXIT%.
  echo Error log: %TEMP%\book2-start-errors.log
  pause
)
exit /b %BOOK2_EXIT%
