@echo off
net session >nul 2>&1
if errorlevel 1 (
  echo Ejecuta este archivo como ADMINISTRADOR.
  pause
  exit /b 1
)
"%SystemRoot%\System32\regsvr32.exe" /u "%~dp0AngelASIO.dll"
pause
