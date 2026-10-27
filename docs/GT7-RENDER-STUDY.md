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

## Last cycle: package headers move, a persistent texture object

- With up to 64 places per T#, race 1 found texture `0x12b8336000`'s T# at `0x12b72b3e40` and
  `0x12b72b4060`: package headers just below the texture data again, at **different addresses**
  than the session before (the texture moved too), and **texture `0x127efd2000`'s T# in GT7's
  heap at `0xe417d43210`**.
- Watches armed on those three addresses before race 2's load (T#-shaped writes only) logged
  **nothing**: race 2 put its texture packages elsewhere (the old header addresses read as zero),
  and the heap object **kept the same T# across both races**. That texture's object is loaded
  once (at boot or in the menus) and reused, not reloaded per race.
- Two more per-frame copiers of T#s: `memcpy` from `0x90004d3a5` and `0x902de0f3f` (called from
  `0x902deb912`).

## Summary of the texture side

- Material and track texture T#s are **prebuilt and stored with the textures**: in per-race
  texture package headers in GPU memory just below the texture data (moved between races) and, for
  textures kept across races, in engine objects in the game heap (`0xe4…`).
- Every frame, CPU code (`0x900188ad0` and others) copies them **through pointers** into per-draw
  records in per-frame scratch memory, which is what Kyty walks for each draw. None of it is
  GPU-written.
- **For a fast path:** the per-draw copies point back at a small set of persistent descriptors
  whose addresses are stable for a race (headers) or longer (heap objects). Keying Kyty's texture
  tracking by those source addresses (hooking `0x900188ad0` and the other copiers, which see both
  the source pointer and the record) would let it skip rediscovering textures per draw. Who writes
  the headers at load (file loader or engine) is still open: the headers move between races, so
  it needs a watch armed from the T# search of the same load, or on the copy that reads the file.

## T#s come prebuilt in the texture package files (file reads, KYTY_MAP_TEXTURES=1)

- With the file-read log (`Common::FileReadLog`: every APR read and pread, by the guest memory it
  filled) and matching on a T#'s first 8 bytes, texture `0x12b8136000`'s T# was found in its
  package header at `0x12b70b4060`, filled by a read of **`contents/K/6/61AOB`** (0x9245e0 bytes
  into `0x12b7086000`). The file holds, at offset `0x2f060`, **`00000000 cb500000 83ffc1ff
  905c0fac`**; memory holds `12b81360 cb500000 83ffc1ff 905c0fac`. **The T# is prebuilt in the
  package file with its base address left 0**, and dword 0 (`0x12b8136000 >> 8`) is filled in after
  the read: format, size and swizzle come straight from the file.
- Other homes of the busiest T#s: a **CPU-side material table** in game memory at `0xec07…`
  (objects 0x1240 bytes apart, each with T#s; that memory had earlier held a file buffer, so the
  file bytes listed for it are stale) and **engine texture objects in the `0xe4…` heap** (five
  places for texture `0x1285082000`).

**Next:** `KYTY_MAP_TSHARP_TEMPLATES=1` scans file reads into the GPU memory range for T#
templates (dword 0 zero, GT7's format bits) and watches the first four, so the code that fills
in the base address (the texture loader) is logged.

## The texture loader's relocation (KYTY_MAP_TSHARP_TEMPLATES=1)

Watches on the first T# template of the first texture packages read at boot (`contents/H/H/PTE7Q`,
`W/U/PVUBV`, `Z/L/6T7DF`) caught the base address being filled in, both times by
**`0x900688100`** (272 bytes), which relocates a texture package object (`rdi`):

- `[rdi+0x1c]` (word): the number of textures; `[rdi+0x20]`: an array of **0x48-byte texture
  records**, each starting with the **prebuilt 32-byte T# from the file**, with a chunk index (word)
  at `+0x36`.
- `[rdi+0x28]`: an array of **0x30-byte memory chunk records**; `+0x28` of each is the GPU address
  the chunk's data was placed at.
- For each texture: `address = chunks[record.chunk].address`; T# dword 0 = `address >> 8`, T# byte
  4 = `address >> 40` (the 40-bit base field); `record+0x1c` = 0.

Callers: `0x900688fe0` (2.7 KB; most likely "finish loading a package") ← `0x90033e810` ←
`0x90068a820` (12 KB) ← `0x90068a6d0` ← `0x90057c680` ← `0x90057c440`.

## Texture side: the whole path

1. A texture package (`contents/…`) is read (APR) into GPU memory; its header holds an array of
   0x48-byte texture records with **prebuilt T#s** (base address 0) and a chunk table.
2. **`0x900688100` relocates it**: the GPU address of each texture's chunk goes into its T#.
3. Engine objects keep pointers to those T#s (CPU-side material tables at `0xec07…`, texture
   objects in the `0xe4…` heap).
4. Every frame, `0x900188ad0` and other copiers copy T#s **through those pointers** into per-draw
   records in per-frame scratch memory, which the shaders' resource tables point at and Kyty walks
   for every draw.

**For a fast path:** hooking `0x900688100` (or its caller) gives every texture of every package,
with its final T#, once, when it loads; the per-draw records then only carry pointers into those
packages. Kyty could register textures at load and key its per-draw lookups by package record,
instead of decoding and validating T#s each draw.

## Texture fast path, stage 1: how many bindings are package textures (KYTY_TEXTURE_REGISTRY=1)

One Northern Isle race, registry fed by APR reads into the GPU memory range:

- 79 packages, 3,493 T# templates registered.
- About 3 million texture bindings every 10 s (non-bindless). Of them:
  - **~5.5 % matched a registered package template** (150 to 270 distinct textures);
  - **~27 % are mipmapped but matched no template** (520 to 850 distinct): most likely packages
    read into CPU memory first (reads into `0xec07…` were seen) and copied or decompressed into
    GPU memory by game code, which the read-based registry never sees;
  - **~67 % are single-level images**: render targets and other runtime images, which no package
    holds.
- **Consequence:** a load-time texture registry can cover at most about a third of the bindings
  (all mipmapped ones), and only if it learns packages from the loader itself (`0x900688100` sees
  every package object, whatever path its bytes took) rather than from file reads. The other two
  thirds are render targets, which need a registry of their own (the engine creates them once; the
  busiest T#s live in the per-frame tables and the game's `.bss`).
- With the texture step at about 1.4 µs of a draw's 9.6 µs, a registry covering a third of the
  bindings would save in the order of 0.3 to 0.5 µs a draw (3 to 5 %): useful only together with
  the larger per-draw costs (the shader resource walk, about 2.6 µs).
