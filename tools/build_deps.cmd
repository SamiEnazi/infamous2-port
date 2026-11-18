@echo off
rem Build glslang into deps\ (tools\fetch_deps.ps1 downloads it): tools\build_deps.py does the work.
rem Every path is quoted, also in each set: a folder name with an ampersand, brackets or a percent sign
rem stays a name (GitHub issues 19 and 34).
setlocal
set "PY=%~dp0..\work\venv\Scripts\python.exe"
if not exist "%PY%" set "PY=%~dp0..\tools-cache\python\python.exe"
if not exist "%PY%" set "PY=python"
"%PY%" -I "%~dp0build_deps.py"
