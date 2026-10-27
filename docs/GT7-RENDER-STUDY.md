# GT7 render submission: feasibility study (2026-10-27)

Can GT7 (PPSA01317 v01.710) be given a game-aware rendering path, one that takes the engine's own
draws instead of the AMD command lists Kyty translates? This records what was measured.

## Method

`KYTY_AGC_CALLSITES=1` (`src/libs/agcCallSites.cpp`) records, for every libSceAgc call, the
function and the guest return address, and reports every 30 s. Function boundaries come from the
eboot's `.eh_frame_hdr` (156,935 functions); callers from a scan of direct `call rel32`
instructions in the code segment. Measured over the menus and a Northern Isle race.

## Findings

- In a race GT7 makes about **486,000 Agc calls a second** (~60,000 draws: ~8 calls a draw) from
  only **155 distinct call sites**.
- **Draws are funnelled through a handful of engine functions:**
  - `0x900005fb0` (9 KB) issues ~95 % of the indexed draws (`sceAgcDcbDrawIndex`, ~55,600/s).
    It is a D3D-style immediate draw call: many arguments, the thread's render context from TLS
    (`fs:0` → `[-0xb8]`), dirty-state bits in it. It is called from only **two** functions:
    `0x900001440` (17 KB) and `0x900072010`; `0x900001440` from two more (`0x9000de170`,
    `0x900364260`).
  - Seven draw functions in all (`DrawIndex` and `DrawIndexAuto` variants at `0x900005fb0`,
    `0x900009bb0`, `0x900018740`, `0x90001a070`, `0x900053930`, `0x9000a1400`, `0x901a3a890`).
- **All seven commit their state through one function, `0x900016370` (5 KB)**, which sets the
  draw's register blocks with `SetUcRegistersIndirect`, `SetCxRegistersIndirect` and
  `SetShRegistersIndirect`: pointers to **prebuilt register blocks** (per material or pipeline),
  not register-by-register writes.
- Shader user data (the resource-table pointers) goes through `sceAgcCbSetShRegisterRangeDirect`
  from one wrapper, `0x901637a90`, called from 150 sites in 59 shader-binding functions
  (`0x9016xxxxx`), about 2.3 times a draw. `sceAgcCreatePrimState` and
  `sceAgcCreateInterpolantMapping2` are called once a draw each (`0x901a33330`, `0x901639050`).

## What it means

- **The draw path is narrow.** A game-aware path could hook seven draw functions and one
  state-commit function, not hundreds of sites. The pipeline state is prebuilt per material, so
  it could be keyed by block address once.
- **The resource side is the hard part.** What makes Kyty's draws cost ~10 µs is rediscovering
  the textures and buffers behind the shader resource tables in guest memory, and tracking CPU
  writes to them. A fast path only removes that by also hooking the engine's resource creation
  and updates (texture, buffer and constant-buffer objects), which are not reached through Agc
  and are not mapped yet.
- **Next steps, if continued:** decode the render context structure used by `0x900005fb0` and
  `0x900016370`; find the engine's texture and buffer creation functions (from the `sceKernel`
  memory and `sceAgc` resource registration calls around them); then prototype a fast path for
  one draw function with the rest left to the command processor.
