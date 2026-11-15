@echo off
rem Fetches RmlUi + FreeType at pinned revisions and builds them as x86 static libraries (/MT, Release) for the
rem in-game overlay (docs/design/eos-transport.md, "In-game overlay"). Sources and output live in the ignored local\:
rem   local\vendor\rmlui, local\vendor\freetype   sources (git clones, pinned below)
rem   local\rmlui-x86\{include,lib}               install prefix that AC4Coop.vcxproj reads
rem Licences: RmlUi MIT, FreeType FTL (BSD-style with credit) - both permit static linking into the ASI.
rem The RmlUi DX11 renderer (Backends\RmlUi_Renderer_DX11.*) is compiled by AC4Coop.vcxproj itself, not here.
setlocal
set "ROOT=%~dp0..\.."
set "V=%ROOT%\local\vendor"
set "OUT=%ROOT%\local\rmlui-x86"
set "RMLUI_COMMIT=bc6ea0ec9968600d0b6e9dc379c5b9ba20f2806d"
set "FT_TAG=VER-2-13-3"
if not exist "%V%" mkdir "%V%"

if not exist "%V%\rmlui\.git" git clone -q https://github.com/mikke89/RmlUi.git "%V%\rmlui" || exit /b 1
git -C "%V%\rmlui" fetch -q --depth 1 origin %RMLUI_COMMIT% || exit /b 1
git -C "%V%\rmlui" checkout -q %RMLUI_COMMIT% || exit /b 1
if not exist "%V%\freetype\.git" git clone -q --depth 1 --branch %FT_TAG% https://gitlab.freedesktop.org/freetype/freetype.git "%V%\freetype" || exit /b 1

set "COMMON=-G "Visual Studio 17 2022" -A Win32 -T v143 -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX="%OUT%" -DCMAKE_PREFIX_PATH="%OUT%""

cmake -S "%V%\freetype" -B "%V%\build-freetype" %COMMON% -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON || exit /b 1
cmake --build "%V%\build-freetype" --config Release --target install -- /m /nologo /v:m || exit /b 1

cmake -S "%V%\rmlui" -B "%V%\build-rmlui" %COMMON% -DRMLUI_FONT_ENGINE=freetype -DRMLUI_SAMPLES=OFF -DBUILD_TESTING=OFF -DRMLUI_PRECOMPILED_HEADERS=OFF || exit /b 1
cmake --build "%V%\build-rmlui" --config Release --target install -- /m /nologo /v:m || exit /b 1

echo.
echo RmlUi %RMLUI_COMMIT% + FreeType %FT_TAG% installed to %OUT%
dir /b "%OUT%\lib\*.lib"
