@echo off
rem Double-click to play. Options: add -Fullscreen, -Volume 50, -SpuLighting
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\play.ps1" %*
pause
