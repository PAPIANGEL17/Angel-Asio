@echo off
net session >nul 2>&1
if errorlevel 1 (
  echo Ejecuta este archivo como ADMINISTRADOR: clic derecho - Ejecutar como administrador.
  pause
  exit /b 1
)
"%SystemRoot%\SysWOW64\regsvr32.exe" "%~dp0AngelASIO.dll"
echo.
echo Listo. Abre tu DAW y elige "AMR Angel ASIO" en Ajustes de audio.
pause
