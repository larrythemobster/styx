@echo off
rem Build every target and run the whole test suite with MSVC 2022.
rem Usage: verify-windows.bat   (BUILD_DIR defaults to build; configure it first with build-windows.bat configure)
setlocal
set REPO=%~dp0..\
if "%BUILD_DIR%"=="" set BUILD_DIR=build
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%REPO%" || exit /b 1
cmake --build "%BUILD_DIR%" --parallel || exit /b 1
ctest --test-dir "%BUILD_DIR%" --output-on-failure || exit /b 1
