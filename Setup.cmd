@echo off
rem Setup: check this PC, get the tools, build inFAMOUS 2 from YOUR copy, make a desktop shortcut.
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0tools\setup.ps1"
