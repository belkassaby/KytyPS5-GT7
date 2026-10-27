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

## Where the bound T#s live (KYTY_MAP_TEXTURES=1)

- A race binds about 6,600 to 8,400 distinct non-bindless T#s. The busiest (each bound about a
  million times a minute) are render targets and global images (`0x120…` addresses).
- In guest memory each lives in **per-frame tables** (a chain of copies from `0x1000015f40` on,
  1,920 bytes apart, or around `0x10000e…`), and the default texture also in the game's `.bss` at
  `0x9063163b8` (the same address in every session; not written during a race load).
- Copies found at `0x7e…` are **the emulator's own heap**, not guest memory: Kyty keeps T#s for
  draw preparation and validation. A write watch armed there fired inside the host allocator and
  ended the session with an uncaught C++ exception; the search and `_mapping/arm.txt` now keep
  to guest-mapped memory (`TryClampRangeSize`).

## Mipmapped (car and track) textures

- Searching guest memory for the eight busiest **mipmapped** T#s (render targets have one level)
  found **seven of them nowhere** in CPU-readable guest memory. The eighth sat at `0x1000000160`,
  which a watch during the next race load showed to be per-frame scratch as well (command buffer
  headers `c0023f00`, floats, `memset`, `memcpy`).
- So GT7's material texture descriptors have **no persistent home the CPU can read**. They are
  either in memory the GPU writes (Kyty keeps GPU-written pages from CPU access, so the search
  skips them: the tables would be built on the GPU, GPU-driven rendering, which fits the ~1,200
  compute dispatches a frame), or the shaders assemble them from parts.
- **Consequence for a game-aware fast path:** the CPU side never holds the full list of material
  textures, so hooking CPU code cannot take over texture tracking by itself. What remains
  hookable on the CPU are the draw calls, the prebuilt state blocks and the render targets.

**Next, if continued:** confirm the GPU-written explanation by logging, for one of those T#s,
the address the SRT walk read it from and whether the GPU wrote it (the buffer cache knows), and
find the compute shader that writes it (`KYTY_GPU_WRITERS=1` names the last GPU writer of a
range).

## Correction: the `0x7e…` addresses (search with protected pages, KYTY_GPU_WRITERS=1)

- `0x7e…` is **guest memory** after all (Kyty's guest-mapping check accepts it): the region where
  Kyty puts the **guest threads' stacks** (`stack_addr = 0x7e0f84000`, 1 MB each). The earlier
  note calling it the emulator's heap was wrong; the Kyty and Windows code that wrote there was
  Kyty working on the game's behalf (or the thread's own calls reusing the stack).
- With protected pages searched too, the eight busiest mipmapped T#s of this session were all in
  CPU-readable memory, **none GPU-written** (no GPU writer recorded): per-frame tables around
  `0x1000608…` and a **table of 32-byte T#s on a thread's stack** at `0x7e0058fd8`, `…ff8`,
  `0x7e0059018`, `…038`, `…058` (no thread GT7 created has its stack there: most likely the main
  thread's). The earlier "nowhere" result (another session, another eight T#s) does not show the
  T#s are built on the GPU; that hypothesis is withdrawn.
- Value-filtered watches on that stack table saw **no rewrite** during the race: it is filled once,
  at race load, in a function that stays active, and copied from into the per-frame tables. Its
  writer is the CPU-side point a fast path would hook.

**Next:** watch the stack table before the next race loads with `KYTY_GUEST_WATCH_TSHARP=1`
(only writes that leave a T#-shaped value are logged), to find the code that fills it.

## Texture package headers and the per-draw record builder

- In another session the busiest mipmapped T#s were again "nowhere": the stack table at
  `0x7e0058…` is live only for part of each frame, so it is not their home.
- Texture `0x12b837e000`'s T# was found at **`0x12b72fae40` and `0x12b72fb060`** (`0x220` apart),
  **in GPU texture memory just below the texture's own data**, readable, not GPU-written: most
  likely the header of a texture package with prebuilt T#s. That memory was **unmapped when the
  race ended** (per-race texture loading).
- A value-filtered watch on a per-frame table caught its writer: GT7's **`0x900188ad0`**
  (callers `0x9001842c0` (16 KB) ← `0x90056f0d0` ← `0x90035cd50`). It takes an 88-byte record from
  a per-frame linear allocator (`0x90000acf0`: size `0x58`, alignment `0x40`) and fills it with two
  32-byte T#s copied **through pointers** (`[r15]` and `*[rbp-0x48]`) and two dwords: a per-draw
  texture record built each frame from persistent descriptors.
- The search stops at 16 places per T#, in address order, so the per-frame copies (`0x1000…`)
  could hide the package headers (`0x12…`) of busier textures; that explains the earlier
  "per-frame copies only" results.

**So far:** texture T#s live in **per-race texture package headers** next to the texture data, and
`0x900188ad0` copies them into per-draw records every frame. A fast path could key textures by
header address (stable for a race) instead of rediscovering them per draw.

**Next:** find what writes the package headers when a race loads (the file loader, so the T#s
are prebuilt in GT7's files, or engine code that patches them), by watching header addresses
before the load (the guest-memory check must accept unmapped addresses in the GPU memory range).
