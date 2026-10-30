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
