@echo off
rem Build the third-party pieces the Vulkan backend needs (into deps\; tools\fetch_deps.ps1 downloads them).
rem   deps\Vulkan-Headers   KhronosGroup/Vulkan-Headers v1.3.296 (headers only)
rem   deps\SDL2-2.30.10     libsdl-org SDL2-devel-2.30.10-VC.zip (+ deps\sdl2-inc\SDL2\*.h)
rem   deps\glslang-src      KhronosGroup/glslang 15.1.0 -> deps\glslang-install (static, HLSL on, no optimizer)
rem Visual Studio, LLVM and Ninja: tools\port_paths.py (config.local.json, or found on this PC).
setlocal
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
set D=%~dp0..\deps
cmake -S "%D%\glslang-src" -B "%D%\glslang-build" -G Ninja "-DCMAKE_MAKE_PROGRAM=%NINJA%" ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_INSTALL_PREFIX="%D%\glslang-install" -DBUILD_SHARED_LIBS=OFF -DBUILD_EXTERNAL=OFF ^
  -DENABLE_OPT=OFF -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF -DENABLE_HLSL=ON ^
  -DENABLE_SPIRV=ON -DGLSLANG_ENABLE_INSTALL=ON || exit /b 1
cmake --build "%D%\glslang-build" || exit /b 1
cmake --install "%D%\glslang-build" || exit /b 1
