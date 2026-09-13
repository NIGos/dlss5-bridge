@echo off
setlocal
if defined VCVARS goto toolchain_found
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo test: Visual Studio Installer vswhere.exe was not found; set VCVARS to vcvars64.bat
  exit /b 1
)
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
:toolchain_found
if not exist "%VCVARS%" (
  echo test: no vcvars64.bat at "%VCVARS%"
  exit /b 1
)
call "%VCVARS%" >nul 2>&1
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl /nologo /W4 /O2 /EHsc /std:c++17 poll-yield.cpp /Fe:poll-yield.exe
if errorlevel 1 exit /b 1
poll-yield.exe
exit /b %errorlevel%
