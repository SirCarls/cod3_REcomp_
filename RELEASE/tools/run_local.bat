@echo off
rem Starts run_local.ps1 (see it for the steps and options). Arguments pass through,
rem for example:  tools\run_local.bat rip -Waw "D:\Games\Call of Duty World at War"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_local.ps1" %*
pause
