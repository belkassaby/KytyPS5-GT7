# GT7 PC port branch (`grant-turismo-7-port-pc-native`)

This branch of [usa-ratt/KytyPS5-GT7](https://github.com/usa-ratt/KytyPS5-GT7) (`gt7-eqaa`) runs
Gran Turismo 7 (PPSA01317, v01.710, from a dump of a game you own) from the command line, and
carries the PC-side improvements ported from the AnyPS5 port (shader precompilation, a graphics
settings dialog, Windows scheduling).

## Build (Windows, no Qt)

Requirements: Visual Studio 2022 Build Tools with the C++ workload and the Clang tools, CMake,
Ninja, and the Vulkan SDK (for `glslangValidator`). Then:

```powershell
git submodule update --init --recursive
.\build-gt7.bat
```

`build-gt7.bat` sets up the x64 build environment and builds `kyty_emulator` with `clang-cl` into
`_Build\windows-noqt` (the Qt launcher is not built).

## Game folder

Kyty loads plain ELFs (and unencrypted SELFs). A dump that keeps the encrypted SELFs next to their
decrypted ELFs (`eboot.bin` + `eboot.elf`, `sce_module\libc.prx` + `libc.elf`, ...) is turned into a
loadable folder of hard links, without copying anything or touching the dump:

```powershell
python tools\gt7\link-game.py D:\path\to\UP9000-PPSA01317-files D:\path\to\GT7-kyty-game
```

## Run

```powershell
.\run-gt7.ps1 -Game D:\path\to\GT7-kyty-game [-Width 1920 -Height 1080] [-PresentMode Mailbox]
```

It passes the settings `docs/GT7.md` lists for the launcher (mailbox presentation, 60 Hz vblank,
performance shader optimization, tessellation, bindless textures, SysV red zone protection,
readback off, printf to `_kyty.txt`).
