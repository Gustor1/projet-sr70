@echo off
rem Le moteur utilise les appels Linux et tourne dans Ubuntu/WSL.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Run
if errorlevel 1 pause
