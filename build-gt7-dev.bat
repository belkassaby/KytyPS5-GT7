@echo off
rem Like build-gt7.bat, into _Build\windows-noqt-dev: builds while a game started from
rem _Build\windows-noqt is still running.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\VulkanSDK\1.4.363.0\Bin;%PATH%
cd /d "%~dp0"
if not exist _Build\windows-noqt-dev\build.ninja (
    cmake -S . -B _Build/windows-noqt-dev -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DKYTY_BUILD_LAUNCHER=OFF || exit /b 1
)
cmake --build _Build/windows-noqt-dev --target kyty_emulator --parallel 16 || exit /b 1
