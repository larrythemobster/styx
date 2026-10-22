@echo off
rem Configure and build Styx with MSVC 2022, Qt 6.12, prebuilt FFmpeg and curl-impersonate.
rem Usage: build-windows.bat [configure]
rem Dependencies are read from %DEPS% (default: ..\deps next to this checkout):
rem   %DEPS%\Qt\6.12.0\msvc2022_64   %DEPS%\ffmpeg   %DEPS%\curl-impersonate
setlocal
set REPO=%~dp0
if "%DEPS%"=="" set DEPS=%REPO%..\deps
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%REPO%" || exit /b 1
if "%1"=="configure" (
  cmake -G Ninja -B build -S . ^
    -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md ^
    -DCMAKE_PREFIX_PATH="%DEPS%\Qt\6.12.0\msvc2022_64;%DEPS%\ffmpeg" ^
    -DCURL_INCLUDE_DIR="%DEPS%\curl-impersonate\include" ^
    -DCURL_LIBRARY="%DEPS%\curl-impersonate\lib\libcurl-impersonate_imp.lib" ^
    -DBUILD_TESTS=ON || exit /b 1
)
cmake --build build --parallel --target acheron || exit /b 1
"%DEPS%\Qt\6.12.0\msvc2022_64\bin\windeployqt6.exe" --release --no-translations build\acheron.exe || exit /b 1
