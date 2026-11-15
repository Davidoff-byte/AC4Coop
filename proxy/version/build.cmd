@echo off
rem Builds the version.dll proxy (x86, static CRT) and its offline test into proxy\version\out\.
rem Needs VS 2022 Build Tools (v143). Run from anywhere.
setlocal
set "HERE=%~dp0"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
call "%VCVARS%" >nul || exit /b 1
if not exist "%HERE%out" mkdir "%HERE%out"
pushd "%HERE%out"
cl /nologo /O2 /MT /W4 /GS /EHs-c- /LD /Brepro "%HERE%version_proxy.cpp" /Fe:version.dll ^
   /link /DEF:"%HERE%version_proxy.def" /Brepro /NOIMPLIB /NOEXP kernel32.lib || (popd & exit /b 1)
cl /nologo /O2 /MT /W4 /Brepro "%HERE%test\mi_test.cpp" /Fe:mi_test.exe /link /Brepro version.lib || (popd & exit /b 1)
del /q *.obj 2>nul
popd
echo built %HERE%out\version.dll and mi_test.exe
