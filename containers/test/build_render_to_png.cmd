@echo off
REM Build render_to_png.exe: standalone litehtml rasterizer for the reftest
REM pixel pipeline. Compiles litehtml + gumbo + the test container + the tool
REM into a single executable (no dependency on the _uv8 Python build).
setlocal
call "E:\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1

set ROOT=E:\Git_clone\uv8\third_party\litehtml
set OUT=%ROOT%\containers\test\render_to_png.exe
set OBJDIR=%ROOT%\containers\test\.obj_rtp
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

set INCS=/I "%ROOT%\include" /I "%ROOT%\include\litehtml" /I "%ROOT%\src\gumbo\include" /I "%ROOT%\src\gumbo\include\gumbo" /I "%ROOT%\src\gumbo\visualc\include" /I "%ROOT%\containers\test"
set FLAGS=/nologo /EHsc /std:c++20 /Zc:__cplusplus /O2 /MD /utf-8 /DNDEBUG

set SRCS=
for %%f in ("%ROOT%\src\*.cpp") do call set SRCS=%%SRCS%% "%%f"
for %%f in ("%ROOT%\src\gumbo\*.c") do call set SRCS=%%SRCS%% "%%f"
set SRCS=%SRCS% "%ROOT%\containers\test\test_container.cpp"
set SRCS=%SRCS% "%ROOT%\containers\test\Font.cpp"
set SRCS=%SRCS% "%ROOT%\containers\test\Bitmap.cpp"
set SRCS=%SRCS% "%ROOT%\containers\test\lodepng.cpp"
set SRCS=%SRCS% "%ROOT%\containers\test\render_to_png.cpp"

echo Compiling render_to_png.exe ...
REM Compile from inside OBJDIR so the .obj files land there (avoids the /Fo
REM trailing-backslash quoting pitfall when one cl invocation compiles many
REM sources at once).
pushd "%OBJDIR%"
cl %FLAGS% %INCS% %SRCS% /Fe:"%OUT%" /link /SUBSYSTEM:CONSOLE > build.log 2>&1
set RC=%ERRORLEVEL%
popd
if %RC% NEQ 0 (
    echo BUILD FAILED - see %OBJDIR%\build.log
    exit /b 1
)
echo BUILD SUCCESS: %OUT%
exit /b 0
