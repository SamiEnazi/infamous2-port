@echo off
rem Build the patched ps3recomp runtime with clang-cl + Ninja (MSVC headers/SDK).
rem   tools\build_toolkit.cmd [Release|Debug] [extra cmake args...]
rem The compile environment: tools\vsenv.py (the portable MSVC in tools-cache, else Visual Studio).
rem CMake, Ninja, LLVM: tools\port_paths.py (tools-cache, config.local.json, or found on this PC).
setlocal
set CFG=%1
if "%CFG%"=="" set CFG=Release
set PY=%~dp0..\work\venv\Scripts\python.exe
if not exist "%PY%" set PY=%~dp0..\tools-cache\python\python.exe
if not exist "%PY%" set PY=python
for /f "delims=" %%i in ('""%PY%" -I "%~dp0port_paths.py" cmake"') do set CMAKE=%%i
for /f "delims=" %%i in ('""%PY%" -I "%~dp0port_paths.py" ninja"') do set NINJA=%%i
if not defined CMAKE exit /b 1
if not defined NINJA exit /b 1
set RUN="%PY%" -I "%~dp0vsenv.py" run
set ROOT=%~dp0..
%RUN% "%CMAKE%" -S "%ROOT%\toolkit" -B "%ROOT%\build\toolkit-%CFG%" -G Ninja ^
  "-DCMAKE_MAKE_PROGRAM=%NINJA%" ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
  -DCMAKE_BUILD_TYPE=%CFG% %2 %3 %4 %5 %6 || exit /b 1
%RUN% "%CMAKE%" --build "%ROOT%\build\toolkit-%CFG%" -- -k 0
