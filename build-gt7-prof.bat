@echo off
rem Like build-gt7.bat, into _Build\windows-noqt-prof, with debug symbols (a PDB next to the
rem emulator) for a sampling profiler; the code is optimized the same way.
set "GT7_CMAKE_EXTRA="-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Z7" "-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Z7" "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/INCREMENTAL:NO /DEBUG:FULL""
call "%~dp0build-gt7.bat" windows-noqt-prof 24
