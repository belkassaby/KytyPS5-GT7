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
