# GT7 parallel execution: audit of the renderer's shared state (2026-10-30)

For GT7-PARALLEL-RENDERER.md's parallel execution: several executor threads, each running whole
draws and dispatches for its own chunk of a frame (a render target run, or a stretch of
dispatches between barriers), each chunk recorded into its own command buffer and submitted in
stream order. This lists what a draw touches that another executor's draw could touch at the
same time, and what each needs.

## 1. The renderer's lock

`RenderExecutor::DrawIndex`, `DrawAuto` and the dispatches hold `RenderContext::GetMutex()`
(`RendererLock`) for the whole command. The presenter takes it (swapchain, window, RenderDoc),
and so do 12 places in `kernel/memory.cpp` (guest map, unmap and protect): a guest memory change
never runs inside a draw.

**Needs:** a reader-writer lock. Executors hold it shared while they run a command; memory
changes and the presenter hold it exclusively. What executors mutate among themselves is
guarded by the caches' own locks (below), not by this one.

## 2. RenderExecutor's members (render.h)

Per draw scratch: `m_graphics_bindings`, `m_compute_bindings`, `m_bound_images`,
`m_descriptor_buffers/images/writes`, `m_image_occurrences`, `m_ms_color_targets`, the mesh
indirect ring (`m_mesh_indirect`, `m_mesh_indirect_next`), `m_indirect_groups`.
**Needs:** nothing: one `RenderExecutor` per executor.

Per frame and shared: the bindless state (`m_bindless_*`: surveyed images, requests, the eager
window, probes, snapshots). **Needs:** it moves to `RenderContext`, under a lock, or bindless
draws stay on the GPU thread (GT7's material draws do not use the bindless heap).

## 3. The command scheduler (commandScheduler.h)

One current command buffer (`Current()`), one rendering state (`BeginRendering`, `EndRendering`),
one recording thread fed by the GPU thread, and the **tick**: each command buffer has a timeline
tick, and the caches mark what a command uses with it (`Image::tick_accessed_last`, buffer
writes in `BufferCache::WritesRecorded`). A game thread that reads GPU memory waits for the tick
that wrote it.

**Needs:** a command buffer per chunk (its own recorder), ticks given out in stream order when a
chunk is cut (so a later chunk's tick is larger, whichever executor records first), and the
submission of chunk command buffers in order. A flush point (a label the guest waits on, a CPU
readback) ends the chunk it falls in.

## 4. The texture cache (cache/textureCache.*)

One spin lock `m_lock` (`TrackingSpinLock`) around every lookup, insertion and removal (47
places), and `m_slots_mutex` (stage 1) for readers off the GPU thread. Fine-grained enough for
correctness today; with N executors every texture lookup contends on `m_lock`.

Mutated per draw **outside** `m_lock`, on shared `Image` objects:

| field | what | order it needs |
|---|---|---|
| `binding.*` (`is_bound`, `is_target`, `needs_rebind`, attachment layout and access) | the image's binding state for the current command buffer | per command buffer |
| current layout and access (`GetBarriers`) | layout transitions, recorded as barriers | **stream order**: a chunk's first use needs the layout the chunk before it left |
| `usage.*` | texture, storage, render target | any (flags only set) |
| `tick_accessed_last`, LRU tick | lifetime | max of the chunks' ticks |
| `gpu_pixels`, `content_serial`, `alias_owner` | GPU writes | stream order |

**Needs:** layouts tracked per chunk. Each chunk records the layout it needs at its first use of
an image and the layout it leaves; when chunks are stitched in order, a barrier between them
moves an image from what the previous chunk left to what the next one needs. This is how
render graph engines record in parallel. Writes (`gpu_pixels`, `content_serial`, owner changes)
applied at the stitch, in order.

Mutations that **create or drop images** (a draw's target or texture over memory with no image,
an overlap resolution that replaces images, uploads from guest memory) are 1 % of prepared
draws or less (milestone 4's audit). **Needs:** a draw that would do one runs on the stitching
side (the GPU thread), in order; the executor stops its chunk there and the rest of the chunk
continues as a new one.

## 5. The buffer cache (cache/bufferCache.*)

No single lock: built for the GPU thread, with `m_dirty_ranges_mutex` for what the fault
handlers share (GPU-modified ranges, fills, metadata watches). Per draw: `FindBuffers`, buffer
syncs (3.8 million in 10 s, 0.22 us each: guest bytes uploaded when the CPU wrote them),
protects (page protection changes, 2.6 us each), and `WritesRecorded` (the tick of GPU writes).

**Needs:** the most work. Syncs and protects are ordered against the guest's writes, which can
only change between submissions, so within one submission they can be done ahead for the whole
chunk on the stitching side; `WritesRecorded` by chunk tick. Buffers are found under a lock.
Dispatches spend 3.8 us of their 7.6 here (buffers), the largest single phase anywhere.

## 6. The program and pipeline caches (pipeline/pipelineCache.*)

Built for concurrency already in parts: 51 mutex uses (asynchronous compiles, the scanner's
walks, `m_graphics_pipelines_mutex`), per-draw snapshots per thread (`StageSnapshot`,
milestone 4). Program entries: replaced while preparing only in full walks (0.1 to 0.7 %).
**Needs:** the entry maps under shared locks for lookups, exclusive for insertions (rare after
warm-up).

## 7. Descriptors (pipeline/descriptors.cpp)

`TextureDescCache`, its front and the texture memo are `thread_local` (each executor has its
own, warmed separately). Descriptor sets: allocated from pools in `CommitBindings`.
**Needs:** a pool per executor. Samplers (`SamplerCache`): lock for creation (rare).

## 8. Diagnostics and statistics

Hundreds of `static` counters and log-once flags across the renderer, mostly atomics already.
GpuPhases, FrameGraph, StateRepeat, DrawMutations, Gt7NativeDraws: thread-local or atomic in
parts, GPU-thread assumptions in others. **Needs:** off by default in executors, or atomics;
not on the critical path.

## The prototype

1. **Chunks, executed on the GPU thread.** Cut the frame into chunks at render target changes
   and barriers, each with a snapshot of the register state at its start; a second
   `CommandProcessor` replays a chunk's packets from its snapshot instead of the main one
   running them. Still one thread, same command buffer. Verify: the replay's draws are the
   main processor's (same packets, same register state). This is the executor's input.
2. **A command buffer per chunk**, stitched in order with the layout barriers of section 4, still
   one thread. Verify: picture and frame rate unchanged.
3. **Two executors**: alternate chunks on a second thread, with sections 1, 4 (image binding
   state), 5 (syncs ahead) and 6 made safe. Mutating draws end their chunk and run in order.
4. Then N executors, and dispatches.

## Slice 1, measured: chunks between packets with effects (2026-10-30, music rally race)

`KYTY_FRAME_SHAPE=1`, "Chunks:" line (`FrameShape::OnPacket`): packets that only set state,
draw or dispatch extend a chunk; any other ends it. About **540 chunks a frame** hold draws or
dispatches (graphics stream and compute queues). Commands by their chunk's length: 1: 8 %,
2-3: 2 %, 4-15: 7 %, 16-63: 21 to 27 %, 64-255: 21 to 33 %, 256 or more: 29 to 37 %.

What ends them, a frame:

| packet | chunks | commands each | what it is |
|---|---|---|---|
| `NOP` / `R_RELEASE_MEM` (0x10/r18) | ~210 | ~6 | an end-of-pipe label write or cache action |
| `EVENT_WRITE` (0x46) | ~180 | ~9 | a cache flush or partial flush (a barrier) |
| `WRITE_DATA` (0x37) | ~90 | 1 | a compute queue's dispatch, then a label |
| `WAIT_REG_MEM_64` (0x93) | ~60 | ~40 | a wait on memory (another queue's label) |

So the frame cannot be cut only where nothing happens: the releases and event writes come every
6 to 9 commands. They are things an executor can do **in order inside its chunk**: a release is
a label written when the work before it completes (Kyty records it against the command buffer's
tick), an event write a barrier in the chunk's own command buffer. The hard boundaries are the
**~60 waits a frame** (cross-queue dependencies), with ~40 commands each, plus the render target
changes. Executors therefore replay release, event and write-data packets themselves; a chunk
ends at a wait, at a render target change, or at a draw that creates or drops images.

## Slice 1, done: chunks replayed from a snapshot (2026-10-30)

`KYTY_REPLAY=1` (`CommandProcessor::ReplayChunk`, graphicsRun.cpp): the GPU thread's command
processor records a chunk's pure packets, runs all but its draws, dispatches and GT7 identity
markers, and at the chunk's end a second processor, started from a copy of its state at the
chunk's start, runs the recorded packets (draw-prep keys restored, the scheduler's command buffer
bound to the replay's registers); the main processor then takes the replay's state back.
`KYTY_REPLAY_VERIFY=1` compares context and user config registers after each replay.

Music rally race and menus: ~8,000 chunks replayed every 10 s (~3.2 million packets, ~750,000
draws and dispatches), **0 states differing** in 24 windows, the picture unchanged. Race windows:
16.3 fps median at 11.8 us a command, as without it.

This is the executor's input: (register state at a chunk's start, its packets) is enough to run
the chunk's draws and dispatches. Next, slice 2: a command buffer per chunk, stitched in order.

## Slice 2 design: a command list per chunk, stitched in order

Reading the scheduler for slice 2: with the recording thread (on by default), a command buffer's
Vulkan commands are already a linked list of recorded lambdas in 256 KB arena chunks
(`CommandScheduler::Record`, `Chunk`), played into the real Vulkan command buffer by the
recording thread. Vulkan recording is not the cost (it is already off the GPU thread), so an
executor does not need a Vulkan command buffer of its own: it needs **a command list of its
own**, spliced into the stream in order. Splicing a linked list is free.

What a chunk's recording needs, one thread first:

1. **Recording target.** `CommandScheduler::Record`, `Stash` and `Allocate` write to the calling
   thread's target: the stream (as now) or a chunk's list. A chunk ends with its rendering ended,
   and the stitch appends its list (and hands its arena chunks to the recording thread).
   Commands recorded through the raw `Handle()` (`DrainRecording`) in a chunk end it and run
   in order on the stitching side.
2. **Image states per chunk** (`Image::GetBarriers`, `backing.state`, `subresource_states`). In
   a chunk, an image's first use takes its state from a chunk overlay instead of the image: the
   use is assumed to find the image as it needs it (no barrier in the chunk) and the overlay
   records that need (layout, access, stage, per subresource where partial). Later uses in the
   chunk go through the overlay as `GetBarriers` does now. At the stitch, for each image the
   chunk used, `GetBarriers` on the image's real state with the chunk's first need makes the
   barriers recorded before the chunk's list; then the image's state becomes the overlay's
   final one.
3. **Rendering and dynamic state.** A chunk begins with no rendering open and an empty
   `DynamicStateShadow` (which drops state calls that repeat what the command buffer holds), and
   ends its rendering; the stitch ends the stream's rendering and resets its shadow before the
   splice.
4. **What stays in order on the stitching side**: anything that records through `Handle()`,
   creates or drops images, or uploads: the chunk ends before it (on one thread, these are
   already in order).

Verify (`KYTY_REPLAY_VERIFY`): with chunk lists on, the GPU thread also runs each `GetBarriers`
against the real image state in stream order, and the stitch checks that each image's state
after the chunk is what the overlay says.

## Slice 2, part 1, done: command lists per chunk (2026-10-30)

`KYTY_CHUNK_LISTS=1` with `KYTY_REPLAY=1`: `CommandScheduler::BeginList` / `SpliceList`. A
replayed chunk records into a list of its own (its own dynamic state shadow and copy batch, no
rendering open at either end), appended to the stream in order at its end; a submit or a raw
`Handle()` drain inside a chunk appends what the list holds first. Image layouts are still
tracked on the images, in stream order (part 2 moves them to per-chunk overlays).

Music rally race and menus: every replayed chunk went through a list (~80,000 every 10 s),
0 states differing in 28 windows, the picture unchanged. Race windows 16.8 fps median at
11.6 us a command (16.3 and 11.8 without lists): ending the rendering at each chunk costs
nothing visible.

## Slice 2, part 2, done: image states per chunk (2026-10-30)

`ImageStateOverlay` (image/image.h): while a chunk records (`KYTY_CHUNK_LISTS=1`,
`KYTY_CHUNK_IMAGES=0` turns it off), `Image::GetBarriers` works on the overlay's copy of each
image's state. A first use in the chunk (per subresource) makes no barrier and is noted, and the
chunk's first first use records a placeholder barrier command into the chunk's list (outside
rendering). At the splice (`SpliceListChunks`), `Stitch` computes the barriers from each image's
real state to the chunk's first needs into that placeholder, and makes the chunk's final states
the real ones; a state the chunk made no barrier for keeps the accesses the real one still waits
for (images:lean). Direct layout reads use `Image::CurrentLayout()`.

Two crashes on the way, both fixed: a stitch recorded through a command buffer `Submit` had
ended (the open list is now spliced before the end), and stitch barriers recorded into the stream
ran between a submit and the next command buffer's begin when a chunk's list started with one
(hence the placeholder inside the list).

Verify (`KYTY_REPLAY_VERIFY=1`: each image's uses replayed in order on its real state and
compared with the stitched state): 32 windows of menus and a music rally race, up to 800,000
images a window, **0 with layouts or accesses differing**, no crash, the picture normal. With the
verify's replays, race windows 16.3 fps median at 13.3 us a command; the A/B without verify is
next. A chunk now needs no image state from the chunks before it: the second executor can record
chunks out of order.

## Slice 2, A/B and the overlay lookup (2026-10-30)

A/B, music rally race windows, same build (09:43): chunk lists and overlays 16.6 fps mean (16.3
median) against 16.9 (16.8) without, the cost in `commit` (2.16 us a draw against 1.23): every
image transition looked its image up in the overlay's hash map. Each image now holds a slot per
executor (`Image::overlay_slots`: the chunk number it is valid for, and the entry's index;
`ImageStateOverlay::Find`), written only by that executor. Verify run after it: 30 windows, 0
states differing, no crash, `commit` 1.05 to 1.36 us a draw.

## Slice 3, split

Two executors need the shared caches safe for concurrent use, the buffer cache first (not
thread-safe at all, used by every draw and by the GPU thread's packets with effects). So:

- **3a: one executor thread.** Chunks replay on it (its own command list and overlay); the GPU
  thread decodes ahead and runs the packets with effects, waiting before each until the executor
  has finished the chunks before it, so no cache is used by two threads at once. Proves the
  threading (hand-off, lists recorded on another thread, splicing in order, overlays per
  executor); gains only the decode overlap (~3 %).
- **3b: the caches safe for concurrent use**: buffer syncs and protects of a chunk done ahead in
  stream order on the stitching side, lookups under locks, image binding state per executor,
  the renderer lock shared, program caches under shared locks.
- **3c: two executors, then N.**

## Slice 3, measured: GT7's queues synchronize ~240 times a frame (2026-10-30)

With releases and event writes inside chunks (`KYTY_REPLAY_SOFT=1`, commit 1dd4dd69: verify run
0 differing states, no hang) chunks did not get longer. The "Chunks:" line with them counted as
inside, and slice ends as their own ender (`ff`), a music rally race:

| ender | a frame | commands before it |
|---|---|---|
| `WAIT_REG_MEM_64` (0x93) | ~170 to 240 | 12 to 20 |
| `NOP` / `R_ACQUIRE_MEM` (0x10/r14) | ~130 to 190 | 4 to 6 |
| `WRITE_DATA` (0x37) | ~50 to 65 | 1 |
| slice ends, `DMA_DATA` | a few | |

Graphics stream: ~170 to 215 chunks a frame of 15 to 22 commands; compute queues: ~250 to 300
chunks of ~4. Acquires only make a global barrier (`CpOpAcquireMem`): they go inside chunks too.
The waits do not: each is the graphics stream waiting for a compute queue's label (or the
reverse), read on the CPU when the wait runs, written by a release in the other queue's stream.
GT7 hands work between its graphics and compute queues ~200 times a frame.

So the frame splits naturally **by queue, not by chunk**: between two waits, the graphics
stream's chunk and the compute queues' chunks are independent by GT7's own design (the waits
are the dependencies, as on the console, where async compute runs beside graphics). Within one
queue, chunks between waits are 15 to 22 commands: too short to split across executors.
