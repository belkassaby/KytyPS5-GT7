# GT7: GPU-driven rendering (plan, 2026-10-30)

Everything per draw moves to the GPU. The CPU-side approaches (a faster translation, parallel
execution, prepare-ahead, native records: GT7-PARALLEL-RENDERER.md, GT7-NATIVE-BACKEND.md) all
leave one CPU thread taking, checking and applying every draw, ~11 us each, ~5,000 commands a
frame. GPU-driven rendering removes the per-draw CPU work instead of spreading it: the GPU resolves
each draw's resources and issues its own draws.

## What the hardware and driver offer (vulkaninfo, RTX 4090 Laptop, driver 621.01)

| | |
|---|---|
| `VK_EXT_device_generated_commands` | the GPU writes a buffer of draw sequences (pipeline switch, push constants, index and vertex buffers, draw), executed as commands: up to 16,384 pipelines in an execution set, 4,194,303 sequences per execute, 32 tokens per sequence; all stages including mesh and compute |
| `VK_EXT_descriptor_buffer` | descriptors in plain GPU memory, no per-draw sets |
| `VK_KHR_buffer_device_address` | buffers read by 64-bit address |
| `VK_EXT_mesh_shader`, `VK_EXT_shader_object` | |
| also `VK_NV_device_generated_commands` (+ `_compute`) | |

## What Kyty has already

- **The BDA page table** (`BufferCache::GetBdaPageTableBuffer`, descriptor binding kind
  `BdaPagetable`): guest page -> device address, kept by the buffer cache; shaders already read
  guest memory through it.
- **The bindless table** (`BindlessTable`, set 1 of bindless pipelines): typed image arrays
  (2D, 2D array, cube, 3D), samplers, a key -> slot translation the host fills, and runtime tables
  and requests through which shaders look up and request textures by descriptor. Used today for
  the guest's own texture heaps.
- **GT7 engine knowledge** (GT7-RENDER-FRONTEND.md, GT7-NATIVE-BACKEND.md): each draw's identity,
  state key and material at the marker; material textures fixed at load
  (`gt7_ResolveMaterialTextures`); the user data equal to the draw's SGPRs (N3).

## The design

1. **Shaders resolve their own resources (GPU SRT).** A recompiler mode in which a shader's
   resources come from its user-data SGPRs at run time, on the GPU: buffer descriptors (V#s) read
   from guest memory through the BDA page table and used as device addresses; texture descriptors
   (T#s) turned into bindless slots through the translation table; samplers likewise. No
   descriptor walk on the CPU, no per-draw descriptor set: a draw needs only its user data.
2. **All textures bindless.** Every image a covered draw can sample is in the bindless table,
   keyed by its T#: GT7's material textures are registered at load (the material registry knows
   them), render targets and the frame's dynamic images (light grids, shadow maps) as the texture
   cache makes them.
3. **Per-draw records in GPU memory.** For each covered draw: pipeline index (from the state key
   and material, N1), the two stages' user data, index buffer and range, instance count, dynamic
   state (viewport, scissor, depth bias). Written by the GPU thread as it decodes (cheap: it
   already parses every packet, ~0.4 us a command) or by GT7's render threads (the records).
4. **Device-generated commands per render target run.** Each run of covered draws (~85 a frame, 79 %
   of draws in runs of 256 or more) is one `vkCmdExecuteGeneratedCommandsEXT`: the GPU switches
   pipelines, pushes the user data, binds the index buffer and draws, for every record. The CPU
   records only the run's rendering, barriers and the execute.
5. **What stays on the CPU**: images and buffers made or uploaded (the caches, as now), render
   target changes and barriers between runs, pipeline compiles (the execution set grows as
   pipelines appear), the guest's labels and waits, and draws not covered (the translation, as now).

## Phases, each verified before the next

- **G0, the mechanism in Kyty.** Enable the extensions; an indirect execution set with the
  pipelines of covered draws; for a run of marked draws, the GPU thread writes their sequences
  (from what its translation produced: pipeline, push data, index buffer, draw) into a buffer and
  executes them with one generated-commands call instead of recording each. Same picture; proves
  the path and measures what it costs.
- **G1, the GPU SRT.** The recompiler mode; programs in it take only user data; verified against
  the CPU walk (the same descriptors resolved).
- **G2, bindless everything** for covered draws.
- **G3, the GPU thread stops preparing covered draws**: decode writes records; one execute per run.
- **G4, coverage**: dispatches (GT7's ~1,900 a frame) the same way, through compute sequences.

## G0, first: the device takes device-generated commands (2026-10-30)

`KYTY_GT7_GPU_DRIVEN=1`: `VK_EXT_device_generated_commands` is enabled with its features
(`deviceGeneratedCommands`, `dynamicGeneratedPipelineLayout`; `GraphicContext::
device_generated_commands_enabled`), and a probe at GPU start makes an indirect commands layout
of one indexed draw a sequence: the driver made it (`VK_SUCCESS`).

What G0 found about the order of the work: a generated-commands execute can switch pipelines
(execution sets), push constants, index and vertex buffers, and draw, per sequence; it cannot bind
descriptor sets. Today every Kyty draw has its own descriptor set (`CommitBindings`), so draws
cannot be batched into one execute until their resources no longer come from per-draw sets:
**G1 (shaders resolving their resources from user data on the GPU) comes before any batching**,
and is the core of the work.

## G1, design: the draw table binding mode (2026-10-30)

G1 is a binding mode in which a shader finds every resource through a **per-draw record in GPU
memory**, located by **a push constant**, so a draw needs no descriptor set of its own. Once a
covered draw's programs run in this mode, its commands are: bind the pipeline, push the record
offsets, bind index buffer, draw: exactly what a generated-commands sequence can do.

### What the recompiler does today (what G1 changes)

| | today | where |
|---|---|---|
| the stage's inputs | *shader data*: the user-data SGPRs it reads, then each buffer's byte offset (8 bits packed, the misalignment under the 256-byte storage alignment), each buffer's length in dwords, write-tracking words | `BindingLayout` (`AllocateBindings`), read by `EmitShaderDataDwordLoad`: push constants when they fit in the 32 shared dwords, else a per-draw `ShaderData` storage buffer |
| buffers | set 0 binding `Buffers`: an array of storage buffers (with u8 / u16 / u64 aliases), one per buffer resource, each the buffer cache's `VkBuffer` at a 256-aligned offset | `PrepareStorageBufferResourceAccess`: `buffers[i]`, `byte_offset` = shader data offset (added to the guest byte address), `length` = shader data length (bounds check) |
| images | set 0, 48 typed image arrays (sampled / comparison / storage x float / uint / sint x dimension) | `DefineDescriptors`, image binding per resource |
| samplers | set 0 binding `Samplers` | |
| flattened SRT | set 0 binding `FlattenedSrt`: per-draw upload | `flattened_srt_variable`, 11 uses |
| bindless (already) | set 1: image arrays 2D / 2D array / cube / 3D, samplers, translation, runtime T# table | `BindlessTable`, `DefineBindlessImages` |
| guest memory by address | set 0 `BdaPagetable` + `FaultBuffer` (same buffers every draw) | `LoadBda*` |

All of a buffer access goes through one place (`PrepareStorageBufferResourceAccess`: object,
byte offset, length), and all of the shader data through one function (`EmitShaderDataDwordLoad`).
That keeps G1's change to the emitter small.

### The mode

`ResourceSpecialization::draw_table` (part of the permutation key, hashed and cached): the host
asks for it for the stages of covered draws (GT7 marked draws' vertex and pixel stages). A program
in this mode has `BindingLayout::draw_table` set and:

- **Buffers by device address.** Each buffer resource has, in the shader data, its **64-bit
  device address** (the buffer cache's `VkBuffer` address plus the binding's offset: the
  cache's buffers are made with `eShaderDeviceAddress`; `ObtainBuffer(..., needs_device_address)`
  keeps small reads off the stream buffer, which has none) and its length. The access is a
  `PhysicalStorageBuffer` pointer, `address + index * element bytes`, with the `Aligned` operand on
  each load and store; the bounds check is unchanged (`index < length`). No slots to manage, and
  an address is a plain value a render thread can write. Write tracking keeps its words.
- **Images and samplers by bindless slot.** Each image resource has a slot in set 1's array of
  its type; each sampler a slot in set 1's sampler array. The host keeps a slot per image view
  (written once, with the read-only layout) and per `vk::Sampler`, released with the view
  (`BindlessTable::ReleaseViews`). In the first cut, only sampled float images (2D, 2D array,
  cube, 3D; comparison included) without per-mip views qualify; a draw whose image is a render
  target, in a general layout, or of another kind keeps the descriptor path.
- **The record.** A stage's record is its shader data, then its slots, then its flattened SRT:
  `[user data][buffer addresses (2) and lengths][write tracking][image slots][sampler slots]
  [flattened SRT]`. The records live in a **draw table ring** (host-visible device memory,
  retired by tick), bound once per command buffer in a set shared by every table-mode pipeline,
  with the BDA page table and the fault buffer. Push constants hold, per stage group (vertex /
  mesh, pixel, hull, domain), the dword offset of the stage's record. `EmitShaderDataDwordLoad`
  reads `table[record + i]`.
- **Pipeline layout.** Table-mode pipelines share one layout: set 0 = the frame set (draw table,
  BDA page table, fault buffer), set 1 = bindless, 128 bytes of push constants. Bound once per
  command buffer, valid across every pipeline switch: what a generated-commands execute needs
  (and, with `VK_INDIRECT_COMMANDS_TOKEN_TYPE_SEQUENCE_INDEX_EXT`, the push could even be the
  sequence index, with the record offsets in a per-run array).

A program qualifies when it has no GDS, no LDS storage binding, no storage images and images
only of the kinds above; the rest keep today's mode, per stage (a draw can mix the modes until
G1.3, where it cannot: its pipeline is table mode as a whole or not at all).

### Steps, each verified and measured with a race

1. **G1.1, buffers by device address**, the transport unchanged (shader data still in push data
   or the shader data buffer; images, samplers and SRT still in set 0). The host writes the
   addresses from `NativeStorageBuffer`'s binding. `KYTY_GT7_G1=buffers`; `=verify` checks every
   table-mode buffer against the descriptor the set would have had (same `VkBuffer`, the address
   = its device address + aligned offset + adjustment, same length).
2. **G1.2, images and samplers by slot.** `KYTY_GT7_G1=images`; verify: each slot's view,
   layout and sampler equal the set's.
3. **G1.3, the record in the draw table**, the frame set, push constants only: a covered draw
   binds no descriptor set. Verify by picture; measure `CommitBindings` (today 0.9 to 1.7 us a
   marked draw) against the record's writing.

Then G3's batching applies to runs of covered draws. Who writes the records changes later (G3:
GT7's render threads, from the records N3 and N4 built: 98 % of images but the light grid,
99-100 % of buffers); their format does not.

## G1.1, verified: buffers by device address (2026-10-31)

`KYTY_GT7_G1=buffers` (with `KYTY_GT7_NATIVE=1`): marked draws' vertex and pixel stages whose
programs have no indirect buffer are compiled in the draw table mode (`ResourceSpecialization::
draw_table`, set on the GPU thread at the permutation lookup, `ApplyDrawTableMode`; a walk resets
it; the scanner's hash takes it as a salt). Their memory bindings have no descriptor: the shader
data carries each binding's device address (the buffer cache's buffer, `BufferDeviceAddress()`,
plus the bound range's 256-aligned offset), and the emitter makes each element pointer
`PhysicalStorageBuffer` from it (`Aligned` on its loads and stores); offsets, lengths and bounds
checks as before. The shader disk cache's format is 5.

Race with `=verify` (each table-mode binding checked against the descriptor the set would have
had: same buffer, its guest range at the offset the shader adds), first run (every shader
recompiled), 10 s windows:

| | |
|---|---|
| table-mode buffer bindings | 0.86 to 2.1 million |
| null (no buffer: size 0 or outside mapped memory, as before) | 39 to 49 % (78 % in the menus) |
| by device address | all the others; 0 without an address |
| checks | 0.5 to 1.1 million, **0 wrong**, 100 % with the guest range checked |
| shaders that gave up | 0 |
| picture | as before (the user) |

Second race, `=buffers` (no checks), shader cache warm but some pipelines still compiling
(`PipelineCompiler` threads at ~40 %): race frames 8 to 21 fps, **~14 to 15 median**; GPU phases,
10 s windows of 490,000 to 780,000 draws: **9.4 to 11.9 us a draw** (commit 0.64 to 0.85, buffers
0.74 to 0.79). Within the spread of earlier races (16 fps median, 10.9 to 11.0 us a draw, run B):
G1.1 neither gains nor costs measurably, as expected while the draw still writes a descriptor set
for its images, samplers and flattened SRT. A same-day baseline (markers on, G1 off) to compare
exactly is still to run.

Same-day A/B (same build and warm cache; markers on in both; `_g1-buffers-race.txt` against
`_g1-baseline-race.txt`), music rally, windows of 2,000 draws a frame or more:

| | G1 off | `KYTY_GT7_G1=buffers` |
|---|---|---|
| frame rate, median (p25 to p75) | **14.4** (12.8 to 16.8) | **14.4** (11.4 to 15.8) |
| us a draw, race windows of 500,000 draws | 11.1 to 13.8 | 11.1 to 13.7 |
| buffers phase | 0.70 to 0.78 | 0.74 to 0.85 |
| commit phase | 0.87 to 1.2 | 0.79 to 1.1 |

No difference beyond the runs' spread (the buffers phase perhaps 0.05 us more: small reads now go
to buffers with an address rather than the stream buffer). G1.1 is neutral, as intended: its point
is that buffers no longer need the descriptor set.

## G1.2, verified: images and samplers by bindless slot (2026-10-31)

`KYTY_GT7_G1=images` (level 2 of `ResourceSpecialization::draw_table`; level 1 is `=buffers`):
in the draw table mode a program's plain sampled images (`IR::TableImage`: 2D, 2D array, cube or
3D, any numeric class, comparison included; one view; not chosen by a key) and all its samplers
are read from set 1's bindless arrays (`BindlessImageBinding`: the array of the image's
dimension; the sampler array), at slots one dword each in the shader data
(`ImageSlotDword`, `SamplerSlotDword`); they have no set 0 binding. Other images keep theirs.

The host (`CommitBindings`, after the images' layout transitions) writes the slots:
`BindlessTable::TableImageSlot` gives an image view in a layout one slot, written the first time
and released with the view (`ReleaseViews`: repointed to the placeholder, freed once the work
recorded with it is done); `TableSamplerSlot` gives a sampler one slot for good. The shader data
buffer, when the shader data does not fit in push constants, is uploaded after them.
`KYTY_GT7_G1_VERIFY=1` checks each slot holds the view or sampler the draw bound. Shader disk
cache format 6.

Race (first run, every shader recompiled), 10 s windows:

| | |
|---|---|
| image slots used | 1.9 to 4.0 million; new 1,100 to 1,700 (38,600 in the session, reused slots included); none at the placeholder |
| sampler slots used | 1.1 to 2.2 million; all made in the menus |
| checks | **0 wrong** (images and samplers), buffers 0 wrong as in G1.1 |
| shaders that gave up | 0 |
| slot arrays full | never |
| picture | textures as before (the user) |

Frame-rate race, `=images` without checks (`_g1-images-race.txt`; some pipelines still compiling,
`PipelineCompiler` threads at ~55 %), against this morning's two (medians of the 10 s windows of
400,000 draws or more):

| | G1 off | `=buffers` | `=images` |
|---|---|---|---|
| frame rate, median (p25 to p75) | 14.2 (12.5 to 16.3) | 14.4 (11.4 to 15.8) | 14.2 (12.0 to 15.8) |
| us a draw | 12.0 | 11.5 | 11.8 |
| programs / textures / images | 3.35 / 1.97 / 0.65 | 2.87 / 1.96 / 0.64 | 2.94 / 1.97 / 0.66 |
| buffers / commit / other | 0.74 / 1.02 / 1.34 | 0.76 / 0.85 / 1.33 | 0.75 / 1.08 / 1.31 |

Neutral again: the slot lookups in `CommitBindings` cost about what the set 0 image and sampler
writes they replace did. The work per draw is still all there (the walk, the texture resolution,
the transitions, a descriptor set for the flattened SRT and the images that do not qualify);
G1.3 is the step that removes the set.

## G1.3, design: records in the stream ring, one cached set (2026-10-31)

Level 3 of the draw table mode (`KYTY_GT7_G1=records`), refined from the plan above with what
G1.1 and G1.2 showed:

- **The record** is the stage's whole shader data (user data, buffer offsets, lengths and
  addresses, write tracking, image and sampler slots) followed by its flattened SRT. The GPU
  thread writes it into the buffer cache's stream ring (`NativeUpload`: one fixed 64 MB buffer,
  256-byte aligned, retired by tick) after the images' layouts (the slots need them), at the
  place it uploads the shader data buffer today.
- **The push data** of a record-mode stage is one dword, the record's offset in dwords, given out
  by the pipeline's push data cursor as shader data is now (`BindingLayout::PushDwords`), so
  record and other stages mix in one pipeline. Without room for it the stage stays at level 2.
- **The shader** reads its shader data at `ring[push + i]` and its flattened SRT at
  `ring[push + ShaderDataDwords() + i]`, through the `ShaderData` binding, which is the whole
  stream ring; no `FlattenedSrt` binding.
- **The set.** A pipeline whose stages are all record-mode and whose set 0 holds nothing but the
  stream ring, the BDA page table and the fault buffer (`Pipeline::static_bindings`) uses no
  push descriptors: one set per set layout, written once from a persistent pool
  (`DescriptorHeap::Persistent`) and checked against the buffers each draw would write, is
  bound instead of allocating and writing a set per draw. The CPU still prepares the draw as
  before (G3 removes that); what goes is the set's allocation and writes, and the shader data's
  size in push constants.

The "GT7 G1 records" line counts records, their size, and the draws that bound the cached set
(and why the others did not). Verified by picture: the record holds the words the shader data
and flattened SRT bindings held before, from the same code.

## G1.3, verified: records in the stream ring (2026-10-31)

`KYTY_GT7_G1=records` with `KYTY_GT7_G1_VERIFY=1`, first run (every shader recompiled; shader
disk cache format 7), music rally, 10 s windows (`_g1-records-verify.txt`):

| | |
|---|---|
| records written | 500,000 to 590,000, **145 to 155 dwords each** (~600 bytes: ~2 MB a frame of the 64 MB ring) |
| graphics commits of marked draws | 270,000 to 330,000, **98.5 to 99.4 % binding the cached set**; 1 to 13 sets written (new pipeline layouts) |
| buffer, image and sampler checks | 0 wrong (as G1.1 and G1.2) |
| shaders that gave up | 0 |
| picture | as before (the user); 9 to 16 fps while every shader compiled |

A covered draw now allocates and writes no descriptor set and pushes one dword a stage.

Frame-rate race, `=records` without checks (`_g1-records-race.txt`), against this morning's
(medians of the 10 s windows of 400,000 draws or more):

| | G1 off | `=images` | `=records` |
|---|---|---|---|
| frame rate, median (p25 to p75) | 14.2 (12.5 to 16.3) | 14.2 (12.0 to 15.8) | **14.3** (12.0 to 16.3) |
| us a draw | 12.0 | 11.8 | 12.9 |
| buffers / commit | 0.74 / 1.02 | 0.75 / 1.08 | **0.56 / 1.67** |
| commit of a marked draw: descriptor writes / recording and the rest | 0.24 / 0.34 to 0.51 | 0.13 / 0.08 to 0.10 | 0.13 / 0.17 to 0.18 |
| records | | | 515,000 to 545,000 per 10 s, 148 to 156 dwords; cached set 94 to 99 % |

The set work is gone, as intended (descriptor writes and the set's allocation and binding: about
0.4 us of a marked draw's commit), but the frame rate did not move and the commit phase grew by
~0.6 us while the buffers phase lost ~0.2 (the flattened SRT upload moved from `RebindBuffers` into
the record). The record's writing (assembling ~150 dwords a stage, then `NativeUpload` into the
ring, inside the commit's image step) costs more than the set it replaces. Next: time it on its
own and make it cheap (written straight into the mapped ring, one upload for a draw's stages),
before G3.

## G1.3, the record's cost cut (2026-10-31)

Timed on its own ("GT7 G1 records" line), a record cost **0.21 us** to write once written straight
into the mapped ring (no scratch vector, `StreamBuffer::Map`/`Commit` directly), but a stage's
**slots 0.47 us**: two or three atomic counter updates per slot and a hash map lookup per image
and sampler (~16 a stage). The counters are now added once a stage, and a 4,096-entry
direct-mapped cache sits in front of each map (`BindlessTable::m_table_image_hits`, cleared for a
view when it is released; samplers are never released). Slots now **0.24 to 0.28 us** a stage.

Races, `=records`, warm cache, medians of the 10 s windows of 400,000 draws or more:

| | G1 off | first `=records` | record written in place | + slot cache |
|---|---|---|---|---|
| frame rate, median (p25 to p75) | 14.2 (12.5 to 16.3) | 14.3 (12.0 to 16.3) | 14.4 (12.9 to 16.3) | **14.9 (13.8 to 16.4)** |
| us a draw | 12.0 | 12.85 | 12.1 | **11.8** |
| buffers / commit | 0.74 / 1.02 | 0.56 / 1.67 | 0.55 / 1.63 | 0.54 / 1.48 |

G1.3 is now slightly ahead of G1 off: ~0.2 us a draw and ~0.5 fps (within the spread of single
races; the lower quartile moved most, 12.5 to 13.8). The commit phase is still above G1 off's
because it now holds the slots and the record (~0.95 us a draw for both stages) in place of the
set work. This is not the frame rate the plan is after: the CPU still prepares every covered draw
(programs 3.0, textures 1.9 us a draw). G1's result is the format: a covered draw is a pipeline,
an index buffer and two record offsets, which is what G3 batches.

## G3, design: the GPU thread stops preparing covered draws (2026-10-31)

After G1 a covered draw's GPU-side needs are a pipeline, an index buffer, the draw's arguments and
one record a stage. What still costs the GPU thread its ~11.8 us is the preparation of that record,
from the guest's registers, draw by draw. In the last race (medians, us a draw): programs 3.0
(taking and checking the prepared stages), textures 1.9, images 0.7, buffers 0.5, commit 1.5
(transitions, slots, record), find and BDA ~0.6; then targets 0.4, vertex 0.4, pipeline 0.2,
rendering 0.5, emit 0.5, other 1.3. G3 moves the first group, ~8 us, off the GPU thread: GT7's
render threads build the record, the GPU thread checks and uses it.

### What the render thread has, and what it lacks (from N3 and N4)

| record word | today, on the GPU thread | on the render thread |
|---|---|---|
| user data | the SGPRs | **has it**: the engine's shadow, 100 % equal (N3 step 1) |
| program, specialization, flattened SRT | the walk (or the scanner's) | **has it**: `Gt7Prepare`, 97 to 99 % of stages, 0 wrong, with the `ReadSet` of every byte read (N3 step 3) |
| pipeline | lookup by registers | learned by state key and material: determined by them (N1, N2: 0.00 % other) |
| buffer addresses, lengths | `FindBuffers`, `ObtainBuffer` (sync), `NativeStorageBuffer` | the V#s (guest base and size) from its prepared snapshot; the owner buffer by `PeekOwner`, 99 to 100 % right (N4) |
| image slots | `ResolveTexture`, views, transitions, `TableImageSlot` | the GPU thread's bindings published by descriptor, with their stamps: 98 % of pixel stages but the light grid (N4) |
| sampler slots | `SamplerCache`, `TableSamplerSlot` | the S# dwords; the slot only through a map the GPU thread publishes |

Three things only the GPU thread can settle, each with a cheap form:

1. **Buffers' contents.** Today each binding is synchronized at the draw (`ObtainBuffer`: ~7
   syncs a draw at 0.24 us). Memory-reading shaders (`uses_dma`) instead rely on the **BDA walk**
   (`RenderContext::PrepareBda`): every CPU-dirty buffer synchronized once a guest submission and
   again after a wait that passed, which is the console's rule (what the game wrote before
   submitting is visible to its commands). Record buffers take that rule: a record holds each
   binding's **guest** address and length, and the shader turns the base into a device address
   through the BDA page table at its start (one lookup a binding; the page table is the buffer
   cache's own map). The GPU thread only has to make sure a cache buffer covers each range: a
   memo of the ranges seen (the engine's buffers are long-lived, at fixed addresses), with
   `ObtainBuffer` for a new one. Stages that write a buffer stay with the GPU thread (write
   tracking and invalidation are its own).
2. **Images' layouts.** A slot holds a view in one layout. At the marker the GPU thread checks each
   record image's current layout is the slot's and its last write is behind a barrier (a read of
   the image's state, as `Transit` does first); otherwise the draw goes the full way. Material
   textures never move after load; render targets sampled later get their transition from the
   draw that falls back, after which the others pass.
3. **The light grid** (the 2 % image the render thread cannot name): the GPU thread resolves it at
   the marker as now and patches its slot word in the record (the record is host-visible).

### Records in memory

Each render thread writes its records into its own ring of host-visible device memory (persistently
mapped; one per thread, so no locks), retired by frame: the GPU thread publishes the last frame
the GPU finished, and a render thread whose ring is full waits for it (none at ~2 MB a frame).
The marker carries the record's ring and offset; the GPU thread pushes them.

### Steps, each measured as before

- **G3.0, shadow records.** The render thread builds the full G1 record for each marked draw (no
  use yet); the GPU thread, which builds its own in `=records`, compares them word by word. Lines:
  coverage (complete records), mismatches by field, the render thread's cost per record. No
  speed change expected; the answer is how many draws G3 can carry and what is missing.
- **G3.1, records used.** For a covered draw whose record is complete, checked (the `ReadSet` and
  the stamps, the image layouts) and whose buffers are covered, the GPU thread skips
  `PrepareBindings` to `CommitBindings`: it binds the learned pipeline and the cached set, pushes
  the record offsets, binds the index buffer and draws. Fallback per draw to the full path.
  Verify mode keeps building its own record and compares.
- **G3.2, runs.** Consecutive covered draws of one render target run go into a buffer of
  generated-commands sequences (execution-set pipeline index, the two offsets as push constants,
  index buffer, draw) and one `vkCmdExecuteGeneratedCommandsEXT` at the run's end (G0's
  mechanism). The pipelines of covered draws join the execution set as they are made.
- **G4**: the compute passes the same way.

Order of risk: G3.0 tells whether the render threads can name enough of each draw (N4 says yes for
images and buffers; samplers and programs' specializations are new); G3.1 is where the frame rate
should move (the GPU thread's ~8 us a covered draw); G3.2 removes most of the remaining recording.

## G3.0, first race: the render threads' records (2026-10-31)

`KYTY_GT7_G3=shadow` (with `KYTY_GT7_G1=records`, `KYTY_GT7_NATIVE_RECORDS=use`): the GPU thread
publishes each learned stage's record layout (`G3Layout`: user data registers, buffers, table
images and samplers) and the slots it gives image descriptors (with the stamp of the image's
memory) and samplers (by S# and flags); the render thread builds the stage's record from its
prepared stage right after `Prepare` (`Gt7NativeWalk::BuildShadow`); the GPU thread compares it
field by field with the record it writes (`G3Compare`). Race, 10 s windows
(`_g3-shadow-race.txt`):

| | |
|---|---|
| shadows built on the render threads | 490,000 to 590,000, 1.35 to 1.44 us each |
| stages with a learned layout | 99.4 to 99.6 % |
| user data | **100.00 %** equal |
| buffer size | **100.00 %** equal |
| buffer address | 65 to 77 % equal: the rest are null bindings (base in page 0 or size 0), which the GPU thread records as address 0 and the shadow left as the descriptor's base (fixed since) |
| image slots | 91.5 to 91.9 % equal, **8.1 to 8.5 % not published** (the light grids), **0.00 % different** |
| sampler slots | **100.00 %** equal |
| flattened SRT | 99.97 to 100 % equal |
| stages with every word equal | 58 to 63 % (the null bindings' artifact included) |
| frame rate, median | 13.5 (the render threads' preparation and shadows; not used yet) |

So far: no field where the render thread's record is wrong, beyond the null bindings' encoding;
images the GPU thread never bound (light grids) are the one real gap, as N4 found.

Second race, null bindings encoded as the GPU thread does (`_g3-shadow2-race.txt`), 10 s windows of
500,000 to 590,000 stages:

| | |
|---|---|
| user data, buffer address, buffer size, sampler slots | **100.00 %** equal |
| image slots | 91.8 to 92.0 % equal, 8.0 to 8.2 % not published (light grids), **0.00 % different** |
| flattened SRT | 99.97 to 100 % equal |
| stages with every word equal | 58 to 61 % (the pixel stages without a light grid, and the vertex stages) |
| **stages with every word equal but unpublished image slots** | **99.4 to 99.8 %** |
| shadows on the render threads | 1.38 to 1.47 us each |

The render threads can build the whole record of 99.4 to 99.8 % of the marked stages, the GPU
thread patching in the light grid's slot: G3.1's ground is there. A handful of differing words
in the whole run (images and samplers of three pixel shaders, SRT words of three) were logged:
G3.1 checks them per draw (stamps, the `ReadSet`) and falls back.

## G3.1a, verified: buffers from the records, synchronized by the BDA walk (2026-10-31)

G3.1 goes piece by piece: each step the GPU thread skips has side effects to keep (buffers' lives,
texture uploads, layout transitions). First the buffers. `KYTY_GT7_G3=use` (it builds the shadows
too): a marked stage in record mode whose shadow has its program's layout and none of whose
buffers is written takes its buffer ranges from the render thread's record (`FindBuffers`), and
`RebindBuffers` addresses each in the buffer that holds it whole (`BufferCache::
TakeOwnerUnsynced`: the page table's owner, touched and marked used in the tick, **not
synchronized**); the draw calls the BDA walk (`PrepareBda`, as memory-reading shaders do), which
synchronizes the CPU's writes once a guest submission and after a wait that passed. A range no
single buffer holds goes the usual way.

Race with `KYTY_GT7_G1_VERIFY=1` (`_g3-use-buffers-verify.txt`), 10 s windows:

| | |
|---|---|
| marked stages with the record's buffers | 396,000 to 455,000 (**99.3 to 99.8 %**; without: 860 to 3,100) |
| bindings addressed without a sync | 814,000 to 1,012,000; **the usual way: 0 to 48** |
| G1 checks (the bound buffer holds the guest range at the shader's offset) | **0 wrong** |
| buffer syncs on the GPU thread | 2.2 million per 10 s (3.6 million before) |
| find / buffers phases, us a draw | 0.21 to 0.23 / 0.41 to 0.44 (0.29 to 0.32 / 0.55 before) |
| picture | as before (the user) |

The BDA walk's rule (the game's writes visible from its next submission) is the console's, and
the picture shows no stale geometry. The gain is small on its own (~0.2 us a draw): the buffers
were the cheap part. Next, the expensive ones: the images (textures phase ~1.9 us) and the
programs (~3.0 us).

## G3.1b, verified: images from the records' slots (2026-10-31)

Each table image slot keeps the binding it was given (image, view, description, the stamp of its
memory: `g_g3_slot_bindings`); a covered stage's images whose record has a slot take that binding
as a texture memo hit does (registered, not to rebind, stamp unchanged; `RebindImages` then checks
the view as for a memo binding). The record comparison now runs only with `=shadow` or
`KYTY_GT7_G1_VERIFY=1` (in `=use` it cost the commit ~0.5 to 1 us: dozens of counter updates a
stage). Race with checks (`_g3-use-images-verify.txt`), 10 s windows:

| | |
|---|---|
| images taken from the records' slots | 680,000 to 700,000; resolved though their slot was published: **2 to 3** |
| slot checks | 0 wrong |
| a marked pixel stage's texture resolution (key, memo, slow path, bind) | **0.1 to 0.2 us** (1.0 before) |
| us a draw: programs / textures / images / buffers / commit | 2.2 to 2.8 / 1.4 to 2.0 / **0.82 to 0.95** / 0.38 to 0.44 / 1.7 to 2.1 |
| all, us a draw | 10.0 to 12.3 (11.9 in G3.1a's race) |
| picture | as before; frame rate ~15 fps (the user) |

The resolution's saving moved partly into the images phase (memo bindings checked in
`RebindImages`: 0.54 to ~0.9 us). The draw still runs the whole per-draw pipeline: the programs'
take and check (`TakeScanned`, `ReadSet::Validate`), `PrepareBindings`, `RebindImages`,
`RebindBuffers`, the transitions and the record in `CommitBindings`. Each piece replaced inside it
saves a fraction of a microsecond: N4's lesson again (GT7-NATIVE-BACKEND.md). The frame rate moves
only when a covered draw does not enter that pipeline at all.

## G3.2, first attempt: a GPU-thread bypass, and why it does not get there (2026-11-02)

`KYTY_GT7_G3=bypass`: a full-path marked draw leaves, for its key (state key and material),
its pipeline, the stages' record layouts, its rendering, depth state and vertex program, and its
vertex table (which entries of the table the vertex shader's user data points at feed each of
Kyty's vertex buffer slots, with each attribute's offset: Kyty's V# is the table's with the offset
added and the format rewritten). The next draws of the key with whole records skip the per-draw
pipeline: the GPU thread writes the two records from the render threads' shadows, binds the
learned pipeline, the cached set, index and vertex buffers (unsynchronized: the BDA walk), sets
the dynamic state and draws. Races (`_g3-bypass1..4.txt`), the last one, 10 s windows:

| marked draws (~290,000 to 300,000) | |
|---|---|
| learnable on the full path | 276,000 to 288,000 (refused: occlusion queries 12,000, tessellation 7,000 to 24,000) |
| **bypassed** | **0 %** |
| light grid (an image slot the render thread cannot name) | **70 to 71 %** |
| no whole record | 17 % |
| index buffer not in a single buffer | 5.5 to 6.7 % |
| not learned / rendering differs | 4 to 5 % / 2 % |
| frame rate | 13.8 fps median |

Two findings:

1. **The light grid is the blocker.** Nearly every pixel stage samples a light grid that GT7
   writes at a new address in its per-frame memory for each draw: Kyty makes (uploads) a texture
   for it per draw, on the GPU thread. No record made ahead can name it, and resolving it at the
   draw is the per-draw texture work the bypass is meant to remove.
2. **A bypass on the GPU thread is still the GPU thread.** Even working, it records every covered
   draw there (records, binds, dynamic state, draw): cheaper than ~12 us, but the same shape.

What GPU-driven rendering needs instead, in order:

- **The light grid read by the shaders from guest memory**, through the BDA page table, as a
  buffer, not as a texture Kyty makes per draw: the recompiler lowers the light grid's image reads
  (by its descriptor class) to address-based loads, the T# in the record. Then every image of a
  covered draw is a material texture or a target with a fixed slot, and a draw's record is whole
  on the render thread.
- **Device-generated commands for runs of covered draws**: the render threads write each draw's
  record and its sequence (pipeline index, record offsets, index buffer, draw); the GPU thread,
  at the run's end, executes the run with one `vkCmdExecuteGeneratedCommandsEXT` and skips the
  run's draw packets. The pipelines of covered draws join an indirect execution set (one shared
  layout: G1.3's static set).

## G3.2, the bypass working; light grids from guest memory (2026-11-02)

- **Light grids** (a linear R32G32_UINT array GT7 writes per draw, ~70 % of covered draws had
  one): a full-path draw learns each (shader, image) light grid's shape from Kyty's texture
  (width, height, layers, row pitch, layer stride); at level 3 the program is specialized with
  `Image::grid` and its loads read the grid by device address (`ImageRead` lowered to two dword
  loads at base + layer * stride + y * pitch + x * 8; out of range reads zero). The record holds
  the grid's base and shape (`BindingLayout::table_grids`, 7 words), the render thread's shadow
  too (`Gt7NativeWalk::GridWords`). Shader disk cache format 8.
- **Vertex tables**: matched by stride, records and a base at most 4 KB below (Kyty's V# is the
  table's with the attribute offset added and its format rewritten); read by the render thread
  through `FindBacking` (`TryReadGpuCleanBacking` serves the GPU thread only: it refused every
  read on the render threads).
- **Index buffers** no single buffer holds (small CPU-written ones go to the stream ring): the
  usual `ObtainBuffer`.

Races (`_g3-bypass5.txt`, `_g3-bypass6.txt`), 10 s windows:

| | unsynchronized (BDA walk) | synchronized per draw |
|---|---|---|
| covered draws bypassed | 58 to 70 %, 1.7 to 2.1 us each | 51 to 66 %, 2.7 to 3.3 us each |
| full path: buffer range / rendering differs / not learned | 19 to 31 % / 4 to 5 % / 3 to 5 % | 18 to 27 % / 4 to 5 % / 5 to 8 % |
| write faults | **49,000 to 61,000/s, 920 to 950 ms/s in handlers** (18,000/s before) | 20,000 to 49,000/s while shaders compiled |
| picture | **flickering**: world map icons, rally menu, pre-race textures, flashes in the race | flicker gone (the user) |
| frame rate | ~15 (the faults cost GT7's threads what the bypass saved) | not measured (shaders compiling) |

Leaving the bypassed draws' buffers to the BDA walk made them read data one sync older than the
full path's draws of the same objects (the flicker), and the walk's protection of whole regions
made GT7's threads fault on most of their writes. Synchronizing each range as the full path does
(`BufferCache::TakeOwnerSynced`; the light grid's base a device address of the cache's
synchronized copy; images with CPU writes pending go the full path) fixes the picture. It is a
CPU stopgap: the GPU-side answer is the GPU reading guest memory in place (imported host memory,
`VK_EXT_external_memory_host`; `KYTY_HOST_IMPORT_PROBE`), with no copies to synchronize at all.

Warm-cache race with `KYTY_HOST_IMPORT_PROBE=1` (`_g3-bypass7.txt`):

| | |
|---|---|
| covered draws bypassed | **67 to 75 %**, 2.8 to 2.9 us each (full path: buffer range 15 %, not learned 6 to 10 %, rendering differs 3.5 %) |
| GPU thread | busy 82 to 83 %, **idle 17 to 18 %** (it was 90 to 98 % busy): it is no longer the only limit |
| write faults | 28,000 to 32,000/s, ~300 to 340 ms/s in handlers |
| frame rate | 12.8 median (9 to 19; the user: closer to 10) |
| shader work still on the GPU thread | 20 to 670 translations and compiles per 10 s (up to 4.7 s of the 10), compiler threads at ~97 %: permutations still new (grid flags learned as the race goes, draw table levels) |

**Host import probe** (`VK_EXT_external_memory_host`, RTX 4090 Laptop): a 63 MB range of GT7's
memory imported in place, read by the GPU at **5.9 GB/s** (its own memory: 18.6 GB/s), bytes
equal. The GPU can read the game's memory where it lies, at PCIe speed: tens of MB a frame of
CPU-written data cost a few ms of GPU time, with no copies, syncs, protections or faults.

The frame rate is not yet a clean measure of the bypass: shader permutations keep compiling
during the race. Next: (1) why permutations keep appearing (the grid flags and levels should
settle after a run); (2) GT7's CPU-written memory imported and read in place by covered draws,
in place of the per-draw synchronization; (3) the generated-commands runs.

Third race on the same build (`_g3-bypass8.txt`; 606 compiles in the run, against 2,351 and ~5,000):

| | G1 off (2026-10-31) | bypass, synchronized per draw |
|---|---|---|
| frame rate, median (p25 to p75) | 14.2 (12.5 to 16.3) | **15.3 (14.1 to 17.1)** |
| covered draws bypassed | | 61 to 72 %, 2.2 to 2.5 us each |
| GPU thread | ~90 to 98 % busy | **62 to 78 % busy, 23 % idle** |
| us a draw (all draws on the full path, GPU phases) | 12.0 | 9.1 |
| write faults | ~18,000/s | 35,000/s, ~700 ms/s in handlers on GT7's threads |

The bypass takes the GPU thread off the critical path (it waits a quarter of the time), and the
frame rate rises ~8 %, the lower quartile most (12.5 to 14.1). What limits the frame now is on
GT7's side: its threads spend ~0.7 s per second in write faults, which the per-draw
synchronization's protections cause (twice the faults of G1 off). The GPU reading GT7's memory in
place (the probe: 5.9 GB/s) removes those synchronizations and their protections.

## In-place reads of GT7's memory, first attempt: 2 fps (2026-11-02)

`BufferCache::BindForRead` (now behind `KYTY_GT7_G3_INPLACE=1`): a bypassed draw's ranges the GPU
wrote go through the cache's synchronized copy, ranges neither wrote through the cache's copy as
is, and ranges the CPU wrote since the copy are read where they lie: 4 MB windows of GT7's memory
(the backing view, `FindBacking`) imported with `VK_EXT_external_memory_host` (the range's own
pages where a window spans two mappings), dropped at unmaps. Race (`_g3-inplace1.txt`):

| | synchronized (before) | in place |
|---|---|---|
| reads | | clean copies ~80 %, in place ~16 %, synchronized ~5 % |
| bypassed draw | 2.2 us | 591 us at first (imports being made), 7 us later |
| GPU thread, a draw | ~9 us | **86 us** |
| fault handling on GT7's threads | ~0.7 s/s | **1.2 s/s** |
| a store to a GPU-written page handled in place | 65 us | **686 us** |
| frame rate | 15.3 | **~2** while imports were made; 10 to 15 once they settled (bypassed draws 10 to 16 us: thousands of small imports, the 4 MB windows mostly refused, 8 MB imported in all) |

The driver pins the imported pages, and Kyty's write tracking changes their protection all the
time (each sync protects, each CPU write faults): every protection change and fault on pinned
memory became ~10 times as slow. Reading the game's memory in place therefore needs the memory read
in place left out of write tracking altogether: a renderer-wide change of how CPU-written memory
reaches the GPU (the full path synchronizes the same pages for its draws), not a covered-draw one.
Off by default; the bypass reads as before (15.3 fps).

## Next: direct regions (design, 2026-11-02; not implemented yet)

AnyPS5 upstream was measured on GT7 in a separate session (never reached a race; ~47 to 54 us a
draw): the port stays on Kyty. The frame is now limited by write faults on GT7's threads, both
ways (protect churn concentrates in 0xec06020000, 0x125b5e0000, 0x125b770000,
0x143d000000-0x143d250000, 0xf439f80000; mostly "no access" protections of GPU-written pages).

**Direct regions**: a few hot mappings (env `KYTY_DIRECT_REGIONS=<hot addresses>`, at most
`KYTY_DIRECT_MAX_MB` each, the whole mapping found with `FindBacking`) become one cache buffer
each, backed by GT7's own memory imported once (`VK_EXT_external_memory_host`), created only at
the first buffer request in the region (nothing older to merge). Hooks in `BufferCache`:
`Buffer::direct` (a constructor over an imported buffer and memory; the destructor frees them);
`CreateBuffer` makes the direct buffer for a request inside a region; `SynchronizeBuffer` returns
at once for it (so the BDA walk too); `MarkGpuWritten` skips it (GPU writes land in GT7's
memory, no read-back protections); `ObtainBuffer`/`Written`/`Tracked` return it directly; the
garbage collector skips it; a request straddling a region's edge stops with a message (to see if
it happens). Every draw and dispatch, full path and bypass, then reads and writes those pages in
place, never write-tracked: no faults there. Then the generated-commands runs.
