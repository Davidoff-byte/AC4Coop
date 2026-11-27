@echo off
rem Builds the x86 overlay oracle (tools\overlay_test) into local\overlay-test\ and runs it against coop_ui\.
rem Needs third_party\rmlui\build_rmlui.cmd first. Run from anywhere: tools\overlay_test\build_overlay_test.cmd
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set "ROOT=%~dp0..\.."
set "RML=%ROOT%\local\rmlui-x86"
set "BK=%ROOT%\local\vendor\rmlui\Backends"
set "OUT=%ROOT%\local\overlay-test"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /EHsc /O2 /MT /W3 /DAC4COOP_OVERLAY=1 /DAC4COOP_UI_RMLUI=1 /DRMLUI_STATIC_LIB /I"%RML%\include" /I"%BK%" ^
   "%~dp0main.cpp" "%ROOT%\src\overlay_ui.cpp" "%BK%\RmlUi_Renderer_DX11.cpp" "%BK%\RmlUi_Platform_Win32.cpp" ^
   /Fo"%OUT%\\" /Fe"%OUT%\overlay_test.exe" ^
   /link /LIBPATH:"%RML%\lib" rmlui.lib freetype.lib d3d11.lib d3dcompiler.lib user32.lib imm32.lib || exit /b 1
"%OUT%\overlay_test.exe" "%ROOT%\coop_ui" "%OUT%\panel_rgba.bmp" || exit /b 1
"%OUT%\overlay_test.exe" "%ROOT%\coop_ui" "%OUT%\panel_bgra.bmp" --bgra || exit /b 1
