@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl /nologo /W4 /O2 /EHsc /std:c++17 poll-yield.cpp /Fe:poll-yield.exe
if errorlevel 1 exit /b 1
poll-yield.exe
exit /b %errorlevel%
