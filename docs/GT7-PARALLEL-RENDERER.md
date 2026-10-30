# GT7: from 16 fps to 4K60 and 120 (plan, 2026-10-30)

The goal is what a PS5 Pro does with GT7: 60 fps locked at 4K, and 120 fps at 120 Hz without ray
tracing. The RTX 4090 Laptop and the i9-13900HX are far past the console's GPU and CPU, so this
is not about raw power. It is about the work the console never does: a PS5's GPU executes GT7's
command buffers itself, while Kyty translates every command into Vulkan on one CPU thread.

## Where the time goes (music rally race, 2026-10-30)

| | per frame |
|---|---|
| draws | 2,400 to 2,900, ~11.8 us each on the GPU thread |
| dispatches | 1,400 to 1,900, ~8.6 us each |
| GPU thread busy | 86 to 91 % (the GPU itself: ~1 %) |
| frame rate | 15.6 to 19.2 fps |
| write-tracking faults | 47,000 to 54,000 a second, 0.8 to 1.0 s of handler time a second |

**Budgets:**

- 60 fps: 16.7 ms a frame, so about 3.5 us per command on one thread, or today's cost on 4 threads.
- 120 fps: 8.3 ms a frame, so about 1.7 us per command, or today's cost on 7 to 8 threads.

The game's own CPU work, its job threads, physics, culling and command buffer building, must fit
in the same frame. It runs natively and has never been measured on its own here.

## What the small wins showed

Milestones 1 to 4 of the native path (GT7-NATIVE-PATH.md) and step 5's measurements found the
cost spread thin: no phase of a draw is more than a quarter of it. Making each step cheaper gave
4 % at a time (material bindings, the depth memo). They do not close a 3x to 7x gap; only
parallelism does. They found what the parallel design needs:

- 92 to 93 % of marked draws write no shared state: no new images or buffers, no uploads, no
  barriers, no render pass change, no new pipelines.
- A draw's prepared state is per thread, not shared (per-draw snapshots).
- The caches the prepare step reads can be read off the GPU thread under the stage 1 locks.
- Template keys (material, mesh, programs, pipeline) repeat 99.7 % of the time.

## The refactor: a renderer in three stages

Today `CommandProcessor` and `RenderExecutor` do everything for a command, in stream order, on
one thread. The refactor splits that work by what it touches.

1. **Decode, in parallel by command buffer.** PM4 to a list of commands with their full register
   state. Each of GT7's command buffers is decoded on its own worker, and the draw-prep scanner
   already does part of this. The native identity markers name each draw's material and mesh.
2. **Resolve, serial and cheap.** In stream order, on the GPU thread, each command's resources
   (images, buffers, pipelines) are found from their identities and the memos. The barrier and
   render-pass plan is made here, along with every mutation: new images, uploads, layout changes.
   This is the only serial part, so it must be the cheap part: a lookup by identity, with the slow
   resolve only on a change.
3. **Record, in parallel by chunk.** Runs of resolved commands are recorded into Vulkan command
   buffers on workers. With dynamic rendering (`VK_KHR_dynamic_rendering`, core in Vulkan 1.3),
   each chunk is a primary command buffer that begins and ends its own rendering, so chunks need
   no inheritance. They are submitted in stream order, with the barriers stage 2 planned between
   them.

The serial stage sets the frame rate. At 1.7 us a command it reaches 120 fps; at 3.5 us, 60 fps.
Today the resolve parts of a draw (programs, textures, buffers, images, targets) are about 6 to
7 us. They come down through the identities: material, mesh and template keys that repeat frame
to frame, with the per-draw values (constant buffers, transforms, index range) filled in.

## Alongside

- **Write-tracking faults.** About one core's worth of handler time is spent catching CPU writes
  to memory the GPU uses, and it stalls GT7's own threads. From the decompile, GT7's allocators
  say which memory only the GPU writes (render targets, per-frame GPU memory). That memory does
  not need tracking.
- **120 Hz.** GT7 offers its 120 Hz mode when video out reports a 120 Hz display. Kyty's video out
  and vblank must report and pace 120 Hz.
- **4K.** The GPU side of 4K is the 4090's. GT7 picks its render resolution from what video out
  reports. (Per the standing rule, no 4K runs until the user asks.)
- **Later: DLSS.** GT7 has a TAA pass with motion vectors, so DLSS can replace its resolve once
  the frame rate is there.

## Steps, each measured before the next

1. **The ceilings.**
   - A null mode: the command processor keeps labels, end-of-pipe writes and DMA, but issues no
     draws or dispatches. The frame rate it gives is GT7's own CPU ceiling with the faults. If
     that is below 120, it comes first.
   - The shape of a frame: command buffers per frame; draws and dispatches per command buffer and
     per render pass; how many draws depend on register state from an earlier command buffer.
2. **Decode off the GPU thread.** Command buffers decoded on workers into command lists. The GPU
   thread executes the lists instead of PM4, with a verify mode against the direct path.
3. **Resolve and record split.** `RenderExecutor` split so that recording takes only the resolved
   command and touches no cache. Then one worker records, proven correct with the same frame
   rate.
4. **N recording workers,** chunked by render pass and barrier.
5. **Resolve by identity:** templates per material and mesh, so the serial stage costs ~1 to 2 us.
6. **Dispatches** the same way.
7. **Tracking, 120 Hz, 4K, then DLSS.**

## Step 1, measured (2026-10-30, music rally race)

`KYTY_FRAME_SHAPE=1` (`src/graphics/guest_gpu/frameShape.cpp`) and `KYTY_NULL_RENDER=1` or `=file`
(skip draws and dispatches; with `file`, only while `_null-render` exists in the working directory).

**The CPU ceiling.** With null mode on in a race, GT7 runs at **60.0 fps** with the GPU thread
13 to 15 % busy, and write-tracking faults fall from 18,000 to 80,000 a second to ~180 (Kyty's
GPU work causes them, not the game). The picture freezes, as expected, and the game keeps
running. With `-Vblank 120` it still runs at 60: GT7 paces itself to 60 and offers its 120 Hz
mode only when video out reports a 120 Hz display. That is its own piece of work; until then the
ceiling known is 60 or more.

**Decoding is cheap.** In null mode the GPU thread still parses every packet and keeps all
register state for ~3,200 draws and ~1,900 dispatches a frame: about 2.2 ms a frame, ~0.4 us a
command, against ~11 us to render one. **Decode stays serial** (step 2 is dropped): even at
120 fps it is 2.2 ms of 8.3. The parallel work is the renderer's.

**The frame's shape:**

| | per frame |
|---|---|
| graphics submissions | 1, made of **~3,200 chained pieces** (GT7's segmented command buffer) |
| compute submissions | ~62 (about 10 empty) |
| render target runs | ~85; **79 % of draws in runs of 256 or more** |
| dispatches in submissions of 256 or more | ~79 % |
| key state from an earlier submission | none |
| key state from an earlier chained piece | targets and viewport 3 to 4 % of draws, CS program 10 % of dispatches |

The chained pieces are not per thread and not self-contained, so the frame cannot be split by
command buffer. It can be split by render target run: long runs, each a natural chunk of
recording for a worker.

**The plan, revised:** decode serial (register state and a command list), resolve serial (the
mutating parts, in stream order), prepare and record in parallel by chunk (a render target run,
or a stretch of dispatches between barriers). Next, step 3: `RenderExecutor` split so that
recording a draw takes only its resolved command and touches no cache.
