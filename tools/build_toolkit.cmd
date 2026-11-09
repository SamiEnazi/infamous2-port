@echo off
rem Build the patched ps3recomp runtime with clang-cl + Ninja (MSVC headers/SDK).
rem   tools\build_toolkit.cmd [Release|Debug] [extra cmake args...]
rem Visual Studio, LLVM and Ninja: tools\port_paths.py (config.local.json, or found on this PC).
setlocal
set CFG=%1
if "%CFG%"=="" set CFG=Release
set PY=%~dp0..\work\venv\Scripts\python.exe
if not exist "%PY%" set PY=python
for /f "delims=" %%i in ('""%PY%" "%~dp0port_paths.py" vcvars"') do set VCVARS=%%i
for /f "delims=" %%i in ('""%PY%" "%~dp0port_paths.py" ninja"') do set NINJA=%%i
for /f "delims=" %%i in ('""%PY%" "%~dp0port_paths.py" llvm"') do set LLVM=%%i
if not defined VCVARS exit /b 1
if not defined NINJA exit /b 1
if not defined LLVM exit /b 1
call "%VCVARS%" >nul || exit /b 1
set PATH=%LLVM%\bin;%PATH%
set ROOT=%~dp0..
cmake -S "%ROOT%\toolkit" -B "%ROOT%\build\toolkit-%CFG%" -G Ninja ^
  "-DCMAKE_MAKE_PROGRAM=%NINJA%" ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
  -DCMAKE_BUILD_TYPE=%CFG% %2 %3 %4 %5 %6 || exit /b 1
cmake --build "%ROOT%\build\toolkit-%CFG%" -- -k 0
