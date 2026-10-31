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
