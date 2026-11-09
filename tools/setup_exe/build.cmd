@echo off
rem Builds Setup.exe (the beginner Setup wizard) into the repo root with the C# compiler that
rem ships with Windows (.NET Framework 4, C# 5). Double-click it or run it from anywhere.
setlocal
set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not exist "%CSC%" goto nocsc
cd /d "%~dp0"
"%CSC%" /nologo /target:winexe /optimize+ /warn:4 /win32icon:..\setup.ico /resource:..\setup.ico,setup.ico /out:..\..\Setup.exe /r:System.dll /r:System.Core.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll /r:System.Web.Extensions.dll /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll SetupWizard.cs
if errorlevel 1 exit /b 1
for %%f in ("%~dp0..\..\Setup.exe") do echo Built %%~ff
exit /b 0
:nocsc
echo The .NET Framework 4 C# compiler was not found.
exit /b 1
