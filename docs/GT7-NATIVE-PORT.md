# GT7 native port: plan (2026-11-03)

How the GT7 work moves from "the PS5 game on an emulator" to a PC port, across the four areas of a
PS5 to PC port: memory, CPU jobs, async compute and pacing, I/O. It builds on what is decoded and
measured already: GT7-RENDER-STUDY.md and GT7-RENDER-FRONTEND.md (GT7's renderer functions and
memory), GT7-NATIVE-PATH.md and GT7-NATIVE-BACKEND.md (the engine-level draw seam),
GT7-PARALLEL-AUDIT.md (GT7's threads and queues), GT7-GPU-DRIVEN.md (the renderer side, measured
this week), and the runs in `_mapping\` (heap watches, texture registry, per-pc writers).

## What "native" means here

- **GT7's code already runs natively.** The PS5's CPU is x86-64 (Zen 2): Kyty loads the eboot at
  0x900000000 and runs it directly, its threads are host threads. There is no CPU emulation to
  remove.
- **What is emulated is the platform:** the GPU (AGC command buffers, recompiled shaders), unified
  memory (page protection keeps device copies in sync), the kernel (threads, memory maps, events)
  and I/O (AMPR reads, decompression).
- **The GT7 rebuild** (D:\Emu\ps5\GRANTURISMO7\rebuild) is GT7's own code as C, from the Ghidra
  decompile: 154,335 functions generated, replaced a function at a time (`KYTY_GT7_REBUILD`,
  `src/loader/gt7Rebuild.cpp`). It lets the port change what GT7 itself does (where it allocates,
  what it writes where, how it submits) where the platform layer alone cannot. State: one
  function replaced (`gt7_MakeBufferDesc`); 37.7 % of a 1,000-function sample compiles.

So the port is the platform layer made PC-native (Kyty) plus GT7's code changed where its PS5
assumptions cost too much on a PC (the rebuild).

## Where the frame goes now (race, warm cache, 2026-11-03)

15.6 to 16.2 fps median. Not GPU-bound: the GPU thread is 90 % busy, GT7's Job#0 at 100 %, write
faults about 50,000 a second, about 2,500 single uploads a frame with two barriers each; Nsight
shows the GPU idle-stalled, not saturated (GT7-GPU-DRIVEN.md). The first limit is the unified
memory emulation (1), then the CPU side (2).

## 1. Memory: unified to split

**PS5:** CPU and GPU share memory; a CPU write is visible to the GPU at once.

**Now:** guest memory is the CPU's. The buffer cache keeps device-local copies, which the GPU reads
by address (BDA page table); a host-visible stream ring takes small CPU-written data. What is
CPU-written is found by page protection: each CPU write after a sync faults, and the next GPU use
stages and copies the page.

**GT7's memory, as decoded and measured so far:**

| Area | What lives there | Written by | Source |
|---|---|---|---|
| 0x1000000000 to ~0x1005000000 | per-frame memory: command buffer memory (`dcb.cursor_down` allocations), per-draw constant blocks (material blocks copied by `gt7_BindMaterial`, copy-on-write stage constants), user data tables (`gt7_WriteUserData`), per-frame T# tables and texture heaps (29 heaps of 31 entries), light grids | GT7's render threads, every frame | GT7-RENDER-FRONTEND.md, GT7-RENDER-STUDY.md, `_mapping\run1-heapwatch.txt`, `run7-protected.txt`, `KYTY_IMAGE_FAULT_STATS` |
| 0xec07... (0xec06... hot) | CPU-side material objects and their constant blocks (T#s filled once a load by `gt7_ResolveMaterialTextures`) | the CPU; no GPU writes measured | GT7-RENDER-FRONTEND.md, `KYTY_GPU_WRITE_STATS` |
| 0x125b400000 (4 MB), 0x143d000000 (2.4 MB) | GPU working memory: ~2 GB of stores a race from eight compute shaders, CPU writes in the same pages | both | `KYTY_GPU_WRITE_STATS` |
| 0xf438800000 (28 MB) | CPU structures, six pages of small GPU-written records; 0xf439f8... read by Job#0/Job#3 | mostly the CPU | `KYTY_GPU_WRITE_STATS` |

**Tried (GT7-GPU-DRIVEN.md):** GT7's memory imported for in-place GPU reads (pinned pages made
every fault and protection change ~10x slower: 2 fps); direct regions (imported once, never
tracked: only safe for memory the GPU never writes; GT7's hot blocks mix CPU data with GPU working
memory, and Kyty's GPU runs later than the PS5's); light grids read by address (15.3 to 16.2 fps).

**Plan:**

1. **Classify every mapping**: `KYTY_MAP_LOG=1` logs each map with its CPU and GPU access (the
   PS5's `PROT_GPU_READ`/`PROT_GPU_WRITE`), its name and the GT7 code that made it. Joined with the
   table above, the rebuild names the allocator behind each area (the per-frame pool behind
   `cursor_down`, the material pool, the compute scratch).
2. **Upload heap for the per-frame memory** (0x1000000000...): GT7 rewrites it every frame and the
   GPU only reads it. Never write-protected: each frame's used part goes to the GPU in one batched
   copy (one barrier), or with Resizable BAR is written where the GPU reads it. Kyty's GPU runs
   later than the PS5's, so the pool is versioned per frame; GT7's frame ring already double- or
   triple-buffers it (the rebuild confirms how many frames and where the cursor resets).
3. **Device-local heap for GPU working memory** (0x125b..., 0x143d...): never tracked, never read
   back unless the CPU reads it.
4. **Where roles mix in one block** (GPU scratch next to CPU data), the rebuild changes the
   allocator so each role gets its own range.
5. **Reserved virtual ranges** (`KernelReserveVirtualRange`, streaming pools) as sparse buffers:
   later, once streaming is profiled.

Resizable BAR: in Vulkan a DEVICE_LOCAL | HOST_VISIBLE memory type as large as VRAM
(`D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH` is D3D12's write tracking, not ReBAR). The device's memory
types are to be logged at startup; without ReBAR, the batched copy.

## 2. CPU jobs and threads

GT7's job system is its own code (Job#0 to Job#N on host threads); GT7-PARALLEL-AUDIT.md maps its
threads and its ~200 queue handoffs a frame. Kyty maps GT7's threads, priorities and affinity
masks; `common/cpuAffinity.cpp` already reads the core topology with
`GetLogicalProcessorInformationEx`.

**Plan:** first a profile of Job#0 (100 % in a race; `_profile-job0*` from an earlier round is the
starting point) with Tracy (`--profile`) or Nsight Systems: GT7's own work against the emulator's
(fault handlers, kernel calls, waits). Then GT7's heavy threads on performance cores; GT7's job
queues and locks in the rebuild only if the profile shows contention there. Render submission is
already split from recording in Kyty (recording thread, async queue submit); the GPU thread's
per-draw work is what the GPU-driven work and the native backend replace.

## 3. Async compute and pacing

GT7 issues compute on asynchronous queues; Kyty runs them on one graphics queue. **Plan:** after
1 and 2 (the frame is not GPU-bound yet), GT7's compute queues on a Vulkan compute queue where a
trace shows overlap to gain; frame pacing on timeline semaphores; low latency (Reflex) last.

## 4. I/O and decompression

GT7 reads through AMPR (`libs/libAmpr.cpp`). **Plan:** measure first where decompression runs
(the PS5's Kraken unit behind AMPR, or GT7's own code) and what it costs in a race and a load;
GPU decompression (compute, or DirectStorage's GDeflate path) only for what is on the CPU.

## Order

1. Memory: classify (1.1), the per-frame upload heap (1.2), GPU working memory (1.3), the
   rebuild's allocator changes where roles mix (1.4).
2. Job#0's profile (2), then thread placement.
3. Async compute and pacing (3), I/O (4), sparse ranges (1.5), as the profiles call for them.

## Log

- 2026-11-03: `KYTY_MAP_LOG=1` (`kernel/memory.cpp`, `NoteMap`): direct, named and flexible maps
  with CPU/GPU access, name and caller.

## Job#0 profile (2026-11-03, race, 15.6 fps)

`tools/gt7/sample-threads.py --guest-funcs <rebuild>\funcs\index.tsv` (30 s at 1 kHz; guest code
named by GT7 function from the rebuild's index; `_profile-job0-native.txt`):

| Thread | Waiting | GT7's job scheduler loop | GT7 work | Emulator work |
|---|---|---|---|---|
| Job#0 | 14 % (kernel wait) | 34 % (`FUN_9000652f0`, self) | ~27 % | ~25 %: fault handling 18 % (`KytyExceptionHandler`, emulated loads, page watchers), the G1/G3 record walk 10 % (`Gt7NativeWalk::ReadRecordedBacking`) |
| Job#1, Job#2 | 64 % | (all their work runs under it: 73 % inclusive) | ~14 % | allocations, page protections |
| Rendr | 99.6 % | | | |
| Thread_Gpu | 18 % | | | 82 %: the command translation |

`FUN_9000652f0` (3.2 KB) is GT7's fiber job scheduler: `sceFiberGetSelf`/`sceFiberSwitch`, job
queue CAS, `Thrd_yield`, mutex and condition variable; the job threads spin and wait in it for work.
GT7's own work is a small part of its job threads (14 to 27 % of each, ~9 to 17 ms of a 64 ms
frame, spread over the workers, some of it render command building the native renderer replaces:
`gt7_DrawModel_1440`, `gt7_StageCxRegisters`, `gt7_WriteUserData`, `gt7_CreatePrimState_site`
at 5 to 8 % inclusive each). The frame waits on the GPU thread's translation and the emulator's
fault handling, not on GT7: the first thing to replace is the renderer (and with it the memory
emulation of render data), as GT7-NATIVE-BACKEND.md planned. Whether GT7's simulation and render
prep fit 8.3 ms is measured again once those are gone (job threads waiting less).

## Map log, first look (2026-11-03)

32,875 maps in a session to a race (`KYTY_MAP_LOG=1`), by access:

| Base (first) | Size | CPU | GPU | Class |
|---|---|---|---|---|
| 0xe400000000 | 14.7 GB (1,690 maps) | write | read+write | shared pools |
| 0x2900000000 | 722 MB (11,560 maps) | none | read+write | GPU-only: device-local |
| 0x3000000000 | 197 MB (3,145 maps) | write | read | CPU-written, GPU-read: upload |

The callers are Kyty's export thunk (0x14012e60d), not GT7's code: the map entry points are reached
through a thunk, so `__builtin_return_address(0)` stops there. The hot areas (0x1000000000,
0xec0..., 0x125b..., 0x143d..., 0xf43...) are not among these maps: they come through AMPR's AMM
commands (`MapDirect`, `MapAutomaticMemory` in libs/libAmpr.cpp), which are not logged yet. Next:
the guest caller taken from the guest stack, and AMM maps logged with the code that recorded them.

## Native renderer: where the GPU thread's frame goes (2026-11-03)

The native renderer is GT7-NATIVE-BACKEND.md's plan (N1 to N4 done: engine state keys, records
built on GT7's render threads, every resource named), carried on in GT7-GPU-DRIVEN.md as the G3
bypass: a marked draw executed from its record at about 2 us instead of the 8.6 us translation.
What the GPU thread spends a 10 s window of a race on now (`GPU phases`, 115 frames):

| Work | Count / 10 s | us each | GPU thread s / 10 s |
|---|---|---|---|
| draws through the translation | 532,000 | 8.75 | **4.7** |
| GT7's compute dispatches | 225,000 | 9.6 (buffers 4.3: syncs and protections of what they write) | **2.2** |
| tessellated draws | 59,000 | 24.6 | **1.4** |
| bypassed draws (G3 records) | 246,000 | 2.05 | 0.5 |

Of the marked draws, 80.9 % are bypassed; but most draws are **not marked**: about 2,100 a frame
bypassed against about 4,600 translated. The markers come from two engine draw functions
(`gt7_DrawIndex`, `gt7_DrawMeshClusters`); GT-RENDER-STUDY.md lists seven, and the compute passes
come from 59 binding functions. So the native renderer's next steps, by the time they take off the
GPU thread:

1. **Coverage census**: which GT7 functions issue the translated draws and the dispatches
   (`KYTY_AGC_CALLSITES=1`, return addresses named with the rebuild's index), and why each marked
   draw still falls back (11.9 % "buffer range", 3.1 % "rendering differs").
2. **Every draw function marked** and recorded, so its draws take the record path (4.7 s).
3. **Compute passes as records** the same way (2.2 s), then the tessellated draws (1.4 s).
4. Then the records' commands recorded on the render threads (secondary command buffers) and the
   PM4 stream no longer decoded for covered work: the GPU thread left with ordering and submission.

### Coverage census (2026-11-03)

`KYTY_AGC_CALLSITES=1`, the busiest call sites named with the rebuild's index (race, per second):

| GT7 function | Calls/s | Native path |
|---|---|---|
| `gt7_DrawMeshClusters` (`AgcDcbDrawIndex`) | 33,800 | marked |
| `gt7_DrawIndex` | 2,300 | marked |
| `gt7_Draw_1a3a890` (`AgcDcbDrawIndexIndirect`) | 7,900 | not marked |
| `gt7_DrawAuto`, `gt7_Draw_9bb0`, `gt7_Draw_53930`, `gt7_Draw_1a070` (`AgcDcbDrawIndexAuto`) | 11,200 | not marked |
| dispatches, `gt7_TireCompute_A` (a compute pass: SH user data, then dispatch) | 10,200 | no |
| dispatches, `gt7_BuildDrawTextureRecord_caller`, `gt7_RenderModel`, ~20 more | 11,800 | no |

### Buffer range fallbacks closed (2026-11-03)

A marked draw fell back to the translation when no single cached buffer held one of its ranges
yet ("buffer range", 18 %): `BufferCache::BindForRead` now makes or merges the buffer as the full
path's binding does (`ObtainBuffer`) instead of refusing. Race: bypassed **88 to 89 %** of the
marked draws (72 % before), "buffer range" 0.0 %, 16.2 fps median (p25 14.8, p75 17.6; 15.6 the
run before). Next: the indirect and non-indexed draw functions marked and recorded, then compute
passes as records.

### Non-indexed and indirect draws on the record path (2026-11-03)

The census's other draw functions write markers too (`gt7NativeDraws.cpp` `OtherDraws`:
`gt7_Draw_1a3a890` with `AgcDcbDrawIndexIndirect`, unless its modifier patches user data;
`gt7_DrawAuto`, `gt7_Draw_9bb0`, `gt7_Draw_53930`, `gt7_Draw_1a070` with `AgcDcbDrawIndexAuto`;
no bound material needed); the command processor's `DrawIndexAuto` takes them as `DrawIndex` does;
the bypass draws a non-indexed draw (`draw`, the full path's offsets) and an indexed one with the
GPU's arguments (`drawIndexedIndirect` after the full path's indirect barrier)
(`TryG3BypassDraw`); learning takes both.

Race: marked draws 373,000 to 407,000 per 10 s (290,000 to 365,000 before), bypassed 75 to 78 %
(about 300,000, from 246,000), states learned ~1,000 (from ~750); not learned 15 %, which are the
draws learning refuses: tessellated 50,000, occlusion queries 37,000, mesh 23,000 per 10 s.
15.8 fps median (p25 14.7, p75 17.1): unchanged, the GPU thread still translates ~470,000 draws
and ~230,000 dispatches per 12.5 s.

### Compute census (2026-11-03)

`AgcCbDispatch` marks each dispatch GT7's eboot issues with the issuing code's address
(`R_GT7_DISPATCH`, `Gt7NativeDraws::ForDispatch`); the GPU thread counts them by (code, compute
shader) with the renderer's time (`NoteDispatch`, a "Compute census" line every 10 s). Race:
226,000 to 257,000 dispatches per 10 s, **all marked**, 8.7 to 9.0 us each, from only **95 to 102
(code, shader) keys** and 70 to 74 codes. By GT7 function (six windows, 11.2 s of GPU thread):

| GT7 function | Dispatches | GPU thread ms | us each | Shaders |
|---|---|---|---|---|
| `gt7_TireCompute_A` | 622,591 | 5,290 | 8.5 | 11 |
| `gt7_RenderModel` | 121,177 | 1,114 | 9.2 | 1 |
| `gt7_TireCompute_B` | 50,367 | 842 | 16.7 | 1 |
| `gt7_TireCompute_C` | 50,367 | 795 | 15.8 | 1 |
| `gt7_BuildDrawTextureRecord_caller` | 139,895 | 629 | 4.5 | 1 |
| `gt7_TireRenderer_Dispatch` | 50,367 | 534 | 10.6 | 1 |
| `FUN_90018b960` | 35,209 | 475 | 13.5 | 1 |
| `gt7_TireCompute_D` | 50,367 | 346 | 6.9 | 1 |
| ~60 more | | ~1,130 | | |

Nine functions and ~20 shaders are 90 % of the compute time: compute records keyed on (code,
shader), learned once each like the draw states, cover nearly all of it. A dispatch's 9 us is
half buffers (4.2 us: syncing and protecting what it writes) and half lookups (program 2.3 us,
bindings ~2.5): the records remove the lookups; the written buffers need the memory split.

### GT7's compute passes named (2026-11-03)

The census's functions, walked up the rebuild's call graph to the RTTI-named classes that call them
(`ghidra-tools\gt7-labels.txt`, applied to the rebuild by `rebuild\tools\rename.py`):

| Name | Was | Evidence |
|---|---|---|
| `gt7_TireCompute_A` | FUN_9016a0660 | under `GTRender::TireRenderer`; 11 shaders, writes through the ACB; 47 % of the compute time |
| `gt7_TireCompute_B`, `_C`, `_D` | FUN_9016abc80, FUN_9016ac630, FUN_9016aa960 | the same caller chain, one shader each, once per tire draw (50,367 each per window) |
| `gt7_TireRenderer_Dispatch` | FUN_9000d23d0 | called by `GTRender::TireRenderer::vf0` alone |
| `gt7_TireRenderer_ComputePasses`, `gt7_TireRenderer_Prepare` | FUN_9000d7a50, FUN_900c47650 | the tire passes' callers under `TireRenderer::vf0` |
| `gt7_RenderModel` | gt7_DrawCaller2_de170 | a model's draws and its per-model dispatch, from `CarGraphics`, `ModelSetRender`, `CourseGraphics`, `TreeInstanced`, `PitWork` |
| `gt7_Grass2_Compute`, `gt7_Grass2_Render` | FUN_902cf84f0, FUN_902d05110 | from `Grass2::Prepare::vf0`, `Grass2::Usual::vf0` |
| `gt7_Sky_TransmittanceCompute` | FUN_9000edf60 | from `(anonymous)::DrawTransmittanceMap::vf0`, 12 shaders |
| `gt7_CommitComputeState`, `gt7_DispatchCompute` | FUN_901a33cc0, FUN_901a34000 | the engine's common compute dispatch (shadows, menus, particles, effects) |

So **the tire renderer is about 70 % of GT7's compute work on the GPU thread** (the four tire
passes and the dispatcher: ~7.8 of 11.2 s in the census windows). The second biggest is the
per-model dispatch of `gt7_RenderModel`. Two of the census's top ten stay unnamed
(`FUN_90018b960`, reached through a function pointer; `gt7_BuildDrawTextureRecord_caller`, kept).
