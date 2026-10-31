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
