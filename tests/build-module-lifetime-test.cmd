@echo off
setlocal
if not defined VCVARS (
  for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
)
call "%VCVARS%" >nul 2>&1
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /W4 /O2 /MT /EHsc /std:c++17 /I..\src\minhook\include /I..\src\minhook\src /LD module-lifetime-fixture.cpp ..\src\minhook.c /Fo:build\ /Fe:build\lifetime-a.dll
if errorlevel 1 exit /b 1
copy /y build\lifetime-a.dll build\lifetime-b.dll >nul
cl /nologo /W4 /O2 /MT /EHsc /std:c++17 module-lifetime-test.cpp /Fo:build\ /Fe:build\module-lifetime-test.exe
if errorlevel 1 exit /b 1
cd build
module-lifetime-test.exe
exit /b %ERRORLEVEL%
