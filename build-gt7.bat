@echo off
rem Builds kyty_emulator (no Qt launcher) with clang-cl from the VS 2022 Build Tools, the way the
rem release workflow does: link-time optimization, AVX2 (x86-64-v3) and the newest profile in pgo\.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\VulkanSDK\1.4.363.0\Bin;%PATH%
cd /d "%~dp0"
set PGO=
for /f "delims=" %%p in ('dir /b /on pgo\*.profdata 2^>nul') do set PGO=%~dp0pgo\%%p
if defined PGO set PGO=%PGO:\=/%
cmake -S . -B _Build/windows-noqt -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DKYTY_BUILD_LAUNCHER=OFF -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON -DKYTY_CPU_ARCH=x86-64-v3 "-DKYTY_PGO_USE=%PGO%" || exit /b 1
cmake --build _Build/windows-noqt --target kyty_emulator --parallel 24 || exit /b 1
