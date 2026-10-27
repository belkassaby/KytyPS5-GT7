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

## Texture heaps (KYTY_MAP_HEAP_WATCH=1)

- A Northern Isle race registers **29 guest texture heaps** with the bindless table, each of
  **31 entries of 188 bytes**, the T# at entry offset `0x9c` (`Heap map:` lines).
- They lie in **per-frame scratch memory**: a plain write watch on one entry was hit by about 150
  code addresses (a `memset` from GT7's job workers, `memcpy`s, physics floats, command buffer
  words). GT7 builds these tables again each frame.
- With `GuestWatch::ArmExpecting` (only writes that leave the T# the entry held), the one writer is
  `0x902df3b1e` in **`0x902df2ee0`** (3.2 KB), which writes a whole record at `rcx`: two 4x4
  float matrices (`+0x00`, `+0x40`), seven float parameters (`+0x80` to `+0x98`) and, at `+0x9c`,
  the **32-byte T# copied from `[rax]`, a persistent texture object**. Its callers: `0x900265890`
  ← `0x9000420f0` ← `0x900041ec0` ← `0x9000459b0` ← `0x9000430f0`.
- The record shape (two transforms, parameters, one texture) looks like a list of **projected
  textures** (decals, light cookies or headlight projections) rather than the general material
  system. Every entry 1 watched held the same T# (`cb5000001282ae10`, a texture at
  `0x1282ae1000`), likely a default.

**So far:** the texture descriptors GT7's shaders read live in persistent engine texture objects
and are copied into per-frame tables. A fast path keyed on those texture objects (found through
`[rax]` in `0x902df2ee0`) could track textures once instead of per draw.

**Next:** trace `rax` back in `0x902df2ee0` to the texture object and its type; watch a texture
object's T# (it changes only when the texture streams in or is replaced) to find the engine's
texture creation and streaming code; and repeat for the per-draw constant and vertex buffers
(the V# side).
