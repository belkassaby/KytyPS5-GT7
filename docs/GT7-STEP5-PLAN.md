# GT7 step 5: a renderer that is not one thread (plan, 2026-10-29)

Goal: GT7's frame rate limited by the RTX 4090 and the CPU's cores, not by Kyty's one GPU thread.
This plan starts from what the earlier rounds measured (GT7-RENDER-STUDY.md,
GT7-RENDER-FRONTEND.md) and from the renderer's structure as it is.

## The budget

A music rally race, `GPU phases` (10 s windows), per frame about **4,200 draws at 10.2 us** and
**1,300 dispatches at 8.6 us**, all on the GPU thread: ~43 ms of draws and ~11 ms of dispatches,
16 to 20 fps. A draw's 10.2 us:

| Phase | us | What |
|---|---|---|
| programs | 2.60 | ~2.5 program gets: key 0.12, the scanner's prepared walk taken and checked 0.66 |
| textures | 1.71 | `PrepareBindings`: `ResolveTexture` (T# cache and memo) and `BindImage` per image |
| commit | 1.16 | `CommitBindings`: the draw's descriptors written to Vulkan |
| other | 1.00 | (describe 0.50) |
| buffers | 0.70 | |
| images | 0.57 | |
| rendering | 0.52 | render pass begin/end bookkeeping |
| emit | 0.43 | the `vkCmdDraw*` and dynamic state |
| targets, vertex, find, pipeline, bda, pending | 1.5 | |

60 fps is 16.7 ms a frame: **~3 us a draw on one thread, or the same work on 3 to 4 threads**.
No single phase is more than a quarter of a draw, so making one step cheaper gives 5 to 15 %
each (the material block cache of step 4 was estimated at 3 to 8 %). Only parallelism closes a
3x gap.

## What constrains it

- **One ordered stream.** The guest's command buffers are executed in order by one command
  processor; draws, dispatches, barriers and render-target changes interleave.
- **Shared caches that are not thread-safe**: `TextureCache`, `BufferCache`, `PipelineCache`,
  `CommandScheduler` (render passes, one Vulkan command buffer). Every phase above uses them.
- **What has been tried:** splitting compute queues to their own thread (no-go: the renderer is
  single-threaded by design); AnyPS5's fork-join draw builds (lost: every join waited, most passes
  are one draw); Kyty's **draw-prep scanner** (works: a second thread walks each draw's resources
  ahead and the GPU thread checks the bytes it read; prefetch of its checks +4 to 7 %).

## The approach: optimistic parallel recording

Most draws in a race should change **no cache state**: their images, buffers and pipelines exist
already, nothing needs uploading, and the render pass is the one open. Such a draw is a pure
function of the guest state plus read-only cache lookups, ending in Vulkan commands. Those can be
recorded on worker threads into **secondary command buffers** (one per worker per render pass),
while the GPU thread keeps everything that mutates (new images and buffers, uploads, barriers,
render pass changes, queries) in stream order and executes the secondaries in order. A draw that
turns out to need a mutation is handed back to the GPU thread (it is recorded after the ones
before it, so order holds).

Milestones, each measured before the next:

1. **How many draws are pure.** Instrument the draw path: per draw, whether it created or
   re-registered an image or buffer, uploaded, synced a buffer, transitioned a layout, changed the
   render pass, or compiled a pipeline. Report per frame the share of draws with none, and what the
   others did. Gate: a large majority (>80 %) pure in steady racing, or the design changes.
2. **Read-only cache paths.** For the pure case, lookups that take no locks and mutate nothing
   (generation-checked, like the texture memo), with a fall-back flag when they would mutate.
3. **One worker, secondary command buffers.** The GPU thread hands runs of pure draws in one render
   pass to one worker; same frame rate expected, but the split proven correct (KYTY verify mode:
   both paths compared).
4. **N workers.** Runs split across workers; then the GPU thread's remaining share sets the limit.
5. **Dispatches** (1,300 a frame, 8.6 us, buffers 4.1 of it) the same way, per compute queue.

Alongside, the step-4 findings keep their value as cheaper per-draw work for whichever thread
does it (material blocks immutable after load, textures resolved per material).

## Further out: a native path

With the engine's draws decoded (GT7-RENDER-FRONTEND.md), a later stage can give Kyty
engine-level identities in stream order: the hooks write a NOP packet with the material and mesh
into GT7's command buffer before a draw, and the command processor keys its caches on them. It
fits inside the same ordered stream, so it composes with the parallel recording rather than
replacing it.
