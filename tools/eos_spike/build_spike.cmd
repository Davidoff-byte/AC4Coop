@echo off
rem Builds the x64 EOS spike into local\eos-spike\ (git-ignored). Needs the SDK in local\eos-sdk\ (git-ignored).
rem Run from the repo root: tools\eos_spike\build.bat
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set SDK=%~dp0..\..\local\eos-sdk\SDK
set OUT=%~dp0..\..\local\eos-spike
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /EHsc /O2 /MT /W3 /I"%SDK%\Include" "%~dp0main.cpp" /Fo"%OUT%\\" /Fe"%OUT%\eos_spike.exe" /link "%SDK%\Lib\EOSSDK-Win64-Shipping.lib" || exit /b 1
copy /y "%SDK%\Bin\EOSSDK-Win64-Shipping.dll" "%OUT%\" >nul
echo built %OUT%\eos_spike.exe
