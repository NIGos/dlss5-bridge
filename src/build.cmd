@echo off
rem dlss5-bridge -- build. The exact line the README quotes; kept here because it
rem has been retyped by hand once too often and lost a flag each time.
setlocal
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
  echo build.cmd: no vcvars64.bat at "%VCVARS%"
  exit /b 1
)
call "%VCVARS%" >nul 2>&1
cd /d "%~dp0"

rc /nologo version.rc
if errorlevel 1 exit /b 1

cl /nologo /W4 /O2 /MT /EHsc /std:c++17 /Ireshade /LD ^
   dlss5-bridge.cpp version.res ^
   /Fe:dlss5-bridge.addon64 ^
   /link /DLL user32.lib advapi32.lib bcrypt.lib
if errorlevel 1 exit /b 1
echo built: %~dp0dlss5-bridge.addon64
endlocal
