@echo off
rem Builds the x64 EOS bridge into local\eos-bridge\ (git-ignored). Needs the SDK in local\eos-sdk\ (git-ignored).
rem Run from the repo root: tools\eos_bridge\build_bridge.cmd
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set SDK=%~dp0..\..\local\eos-sdk\SDK
set OUT=%~dp0..\..\local\eos-bridge
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /EHsc /O2 /MT /W3 /I"%SDK%\Include" "%~dp0main.cpp" /Fo"%OUT%\\" /Fe"%OUT%\eos_bridge.exe" /link "%SDK%\Lib\EOSSDK-Win64-Shipping.lib" || exit /b 1
copy /y "%SDK%\Bin\EOSSDK-Win64-Shipping.dll" "%OUT%\" >nul
copy /y "%SDK%\Bin\x64\xaudio2_9redist.dll" "%OUT%\XAudio2_9Redist.dll" >nul
for %%d in (msvcp140.dll vcruntime140.dll vcruntime140_1.dll) do copy /y "%SystemRoot%\System32\%%d" "%OUT%\" >nul
echo built %OUT%\eos_bridge.exe
