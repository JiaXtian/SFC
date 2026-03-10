@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "BUILD_DIR=%SCRIPT_DIR%\build"
set "BUILD_TYPE=Release"
set "CLEAN_BUILD=0"
set "GENERATOR="

echo ========================================
echo   SFC Backend - Windows Build Script
echo ========================================
echo.

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="--debug" (
  set "BUILD_TYPE=Debug"
  shift
  goto parse_args
)
if /I "%~1"=="--release" (
  set "BUILD_TYPE=Release"
  shift
  goto parse_args
)
if /I "%~1"=="--clean" (
  set "CLEAN_BUILD=1"
  shift
  goto parse_args
)
if /I "%~1"=="--ninja" (
  set "GENERATOR=Ninja"
  shift
  goto parse_args
)
if /I "%~1"=="--help" goto show_help
if /I "%~1"=="-h" goto show_help

echo Error: Unknown option %~1
goto show_help

:show_help
echo Usage: build_windows.bat [options]
echo.
echo Options:
echo   --debug        Build with Debug type
echo   --release      Build with Release type ^(default^)
echo   --clean        Remove existing build directory before configure
echo   --ninja        Force Ninja generator
exit /b 1

:args_done
echo [1/6] Checking toolchain...
where cmake >nul 2>nul
if errorlevel 1 (
  echo Error: cmake not found in PATH.
  echo Install CMake: https://cmake.org/download/
  exit /b 1
)

for /f "usebackq delims=" %%i in (`cmake --version ^| findstr /B /C:"cmake version"`) do set "CMAKE_VER=%%i"
echo   ^| !CMAKE_VER!

if not defined GENERATOR (
  where ninja >nul 2>nul
  if not errorlevel 1 (
    set "GENERATOR=Ninja"
  ) else (
    set "GENERATOR=Visual Studio 17 2022"
  )
)

echo   ^| Generator: !GENERATOR!

if /I "!GENERATOR!"=="Ninja" (
  where cl >nul 2>nul
  if errorlevel 1 (
    where g++ >nul 2>nul
    if errorlevel 1 (
      echo Error: No compiler found for Ninja build.
      echo Install one of: Visual Studio C++ Build Tools ^(cl^) or MinGW ^(g++^).
      exit /b 1
    )
  )
)

echo.
echo [2/6] Resolving ONNX Runtime...
if not defined ONNXRUNTIME_DIR (
  if exist "%SCRIPT_DIR%\..\onnxruntime-win-x64-1.16.0\lib\onnxruntime.lib" set "ONNXRUNTIME_DIR=%SCRIPT_DIR%\..\onnxruntime-win-x64-1.16.0"
)
if not defined ONNXRUNTIME_DIR (
  if exist "%SCRIPT_DIR%\..\onnxruntime\lib\onnxruntime.lib" set "ONNXRUNTIME_DIR=%SCRIPT_DIR%\..\onnxruntime"
)
if not defined ONNXRUNTIME_DIR (
  if exist "C:\onnxruntime\lib\onnxruntime.lib" set "ONNXRUNTIME_DIR=C:\onnxruntime"
)

if not defined ONNXRUNTIME_DIR (
  echo Error: ONNXRUNTIME_DIR not set and default locations not found.
  echo Expected: ^<ONNXRUNTIME_DIR^>\lib\onnxruntime.lib
  echo.
  echo Example setup:
  echo   1^) Download Windows x64 package from ONNX Runtime releases
  echo   2^) Extract to e.g. C:\onnxruntime
  echo   3^) set ONNXRUNTIME_DIR=C:\onnxruntime
  exit /b 1
)

if not exist "%ONNXRUNTIME_DIR%\lib\onnxruntime.lib" (
  echo Error: ONNX Runtime library not found: %ONNXRUNTIME_DIR%\lib\onnxruntime.lib
  exit /b 1
)

if not exist "%ONNXRUNTIME_DIR%\include\onnxruntime_cxx_api.h" (
  echo Warning: cannot find include\onnxruntime_cxx_api.h under ONNXRUNTIME_DIR.
)

echo   ^| ONNXRUNTIME_DIR=%ONNXRUNTIME_DIR%

echo.
echo [3/6] Detecting optional vcpkg toolchain...
set "TOOLCHAIN_ARG="
if defined VCPKG_ROOT (
  if exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
    set "TOOLCHAIN_ARG=-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"
    echo   ^| Using VCPKG_ROOT: %VCPKG_ROOT%
  ) else (
    echo   ^| VCPKG_ROOT is set but toolchain file not found, skip.
  )
) else (
  echo   ^| VCPKG_ROOT not set, skip.
)

echo.
echo [4/6] Preparing build directory...
if "%CLEAN_BUILD%"=="1" (
  if exist "%BUILD_DIR%" (
    rmdir /s /q "%BUILD_DIR%"
  )
)
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

echo.
echo [5/6] Configuring with CMake...
set "PREFIX_PATH=%ONNXRUNTIME_DIR%;C:\Program Files\Drogon;C:\Program Files\spdlog"

if /I "!GENERATOR!"=="Ninja" (
  cmake -S "%SCRIPT_DIR%" -B "%BUILD_DIR%" -G "Ninja" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DCMAKE_PREFIX_PATH="%PREFIX_PATH%" %TOOLCHAIN_ARG%
) else (
  cmake -S "%SCRIPT_DIR%" -B "%BUILD_DIR%" -G "!GENERATOR!" -A x64 -DCMAKE_PREFIX_PATH="%PREFIX_PATH%" %TOOLCHAIN_ARG%
)
if errorlevel 1 (
  echo Error: CMake configure failed.
  echo Hint: install Drogon/spdlog/nlohmann_json and make sure CMake can find them ^(vcpkg recommended^).
  exit /b 1
)

echo.
echo [6/6] Building...
cmake --build "%BUILD_DIR%" --config %BUILD_TYPE% --parallel
if errorlevel 1 (
  echo Error: Build failed.
  exit /b 1
)

if not exist "%SCRIPT_DIR%\data" mkdir "%SCRIPT_DIR%\data"
if not exist "%SCRIPT_DIR%\public" mkdir "%SCRIPT_DIR%\public"

echo.
echo ========================================
echo   ^| Build Complete ^(%BUILD_TYPE%^)
echo ========================================
if exist "%BUILD_DIR%\%BUILD_TYPE%\sfc_server.exe" (
  echo Executable: %BUILD_DIR%\%BUILD_TYPE%\sfc_server.exe
) else if exist "%BUILD_DIR%\sfc_server.exe" (
  echo Executable: %BUILD_DIR%\sfc_server.exe
) else (
  echo Executable built. Locate sfc_server.exe under build directory.
)
echo.
echo Run hint:
echo   set PATH=%ONNXRUNTIME_DIR%\lib;%%PATH%%
echo   cd /d %BUILD_DIR%
echo   sfc_server.exe
echo.

exit /b 0
