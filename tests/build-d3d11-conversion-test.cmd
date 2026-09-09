@echo off
setlocal
rem Build and run the production D3D11 conversion test. No NGX or game is used.
rem --warp selects software D3D11; --build-only skips execution.
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
if not exist build mkdir build
if errorlevel 1 exit /b 1
cl /nologo /W4 /O2 /MT /EHsc /std:c++17 /I..\src\reshade ^
   /Fo:build\ /Fe:build\d3d11-conversion.exe d3d11-conversion.cpp ^
   /link user32.lib advapi32.lib bcrypt.lib d3d11.lib
if errorlevel 1 exit /b 1
if /i "%~1"=="--build-only" exit /b 0
cd build
d3d11-conversion.exe %*
exit /b %ERRORLEVEL%
