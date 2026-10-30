# GT7 native renderer backend (plan, 2026-10-30)

The goal since the start: GT7's renderer drawing through Vulkan directly, not through Kyty's
translation of AMD command buffers. The translation is where the frame rate goes. The RTX 4090
is ~1 % busy while one thread rediscovers ~5,000 commands a frame from raw registers at ~11 us
each. Every attempt to make that thread faster or to split its work found the cost spread thin,
and found GT7's own structure in the way: ~200 queue handoffs a frame (GT7-PARALLEL-AUDIT.md).

## The seam: GT7's render threads, at the engine's draw calls

GT7 builds its frame on several render threads in parallel. Each thread's TLS render context
(`GT7RenderContext`, GT7-RENDER-FRONTEND.md) holds a draw's whole state **in engine terms**
when the engine draws:

| state | where | what the backend makes of it |
|---|---|---|
| render targets | `color_targets[8]`, `depth_target` (register pairs) | a target set (images shared with Kyty's texture cache) |
| fixed function | dirty groups 3 to 16: viewport, blend, target mask, colour control, MSAA, cull, clip, scissor, polygon offset | the pipeline's state, hashed per group |
| shaders | `stages[6]`: shader header per stage | Kyty's recompiled programs (same SPIR-V) |
| constants | `stages[n]`: copy-on-write constant blocks in DCB memory | copied into a ring at draw time |
| textures | the material's block (`gt7_BindMaterial`, filled at load by `gt7_ResolveMaterialTextures`) | descriptor sets per (material, program), made once |
| vertex input | `vertex_buffers[16]` (V#s), formats, `attrib_slots` | vertex buffer bindings, static meshes uploaded once |
| transforms | `transforms`, `bindings` | constants (as now: the commit writes them to SGPRs or constants) |
| the draw | `gt7_DrawIndex` / `gt7_DrawMeshClusters` arguments, `index_size`, `num_instances` | draw arguments |

Kyty reaches the same state from registers, one draw at a time, on its one GPU thread. The
backend builds it from the engine's context, on the thread that made the draw: GT7's render
threads become the backend's recording threads, in parallel by GT7's own design.

## The architecture: native draw records, executed in stream order

1. **On GT7's render thread**, at the draw (the hooks of native milestone 1 already run there),
   the backend builds a **native draw record**: pipeline, descriptor set, vertex and index
   buffers, constants (copied into a ring), draw arguments, and a target set. Everything here is
   keyed on engine identity (material, mesh, shader, state group hashes), which repeats 99.7 % of
   the time: almost every record is assembled from cached pieces.
2. **The marker packet** (`R_GT7_DRAW`, milestone 1), written into GT7's command buffer just
   before the draw packet, carries the record's index. The order of draws in the frame is
   GT7's command buffer order, as before.
3. **On Kyty's GPU thread**, at the marker: the render pass for the record's target set (shared
   with translated draws, which still render into the same images), the barriers the record
   needs (material textures stay read-only after load), then the record's binds and its draw,
   and the draw packet itself is skipped. A target: ~1 to 2 us a draw, against ~11.
4. **Everything not covered yet** (draws the backend cannot build, compute passes, clears,
   copies, post-processing) goes through Kyty's translation as now, interleaved in stream order.
   Coverage grows pass by pass, and the translation layer is the fallback throughout.

What stays shared with Kyty: the shader recompiler and program cache, the texture cache (render
targets and the images both paths use), the scheduler, the presenter. What the backend owns:
pipelines from engine state, material descriptor sets, mesh buffers, the constants ring, and
the records.

## Milestones, each verified before the next

- **N1, shadow records.** Build each draw's engine-level description on the render thread
  (state group hashes, shader headers, material, mesh, constant block addresses and sizes,
  vertex buffers, draw arguments), with no Vulkan, and attach it to the marker. On the GPU
  thread, compare it with what Kyty's translation does for the same draw: the same programs,
  pipeline, textures and buffers. Proves the engine state is complete; measures the cost on the
  render threads.
- **N2, pipelines from engine state.** The record's pipeline built from its state group hashes
  and programs (pipeline creation safe off the GPU thread); verified against Kyty's choice.
- **N3, resources.** Material descriptor sets (textures from the registry, samplers), mesh
  vertex and index buffers, the constants ring. Verified per draw against the translation's
  bindings.
- **N4, native execution.** Verified records drawn natively at their markers; the translation
  for the rest. Verified by picture and by an A/B.
- **N5, coverage.** The remaining draw classes, then the compute passes (by their 59 callers),
  then the post-processing chain; then DLSS on GT7's TAA input, 120 Hz.

## N1, measured: the engine state determines Kyty's choices (2026-10-30)

`KYTY_GT7_NATIVE=1`: a hook on `gt7_CommitDrawState` makes the state key on the render thread
(targets `+0x10..0x718`, attribute slots `+0x770..0x788`, the six stages' shaders, the primitive
type, vertex formats `+0xda4..0xde8`), the marker carries it, and the GPU thread compares each
marked draw's key with the programs and pipeline Kyty's translation chose. Music rally race,
10 s windows of 290,000 to 375,000 marked draws (82 % of indexed draws):

| | |
|---|---|
| cost on the render threads | 0.31 to 0.32 us a commit (~530,000 commits per 10 s, on GT7's threads) |
| state key and material seen before, other programs or another pipeline | **0.00 %** |
| state key alone seen before, other programs / another pipeline | 0.06 to 0.35 % / 0.29 to 1.05 % (programs specialize on the bound textures: the material) |
| keys not seen before (state and material) | 32 to 39 % of the draws in each window |

The engine state with the material **determines** Kyty's programs and pipeline: N1's question
is answered. But a third of the draws bring a key never seen: something in the hashed fields
changes from draw to draw (per-draw values among the target fields, or in the attribute or
vertex format fields). For records built from cached pieces, N2 splits the key into its parts,
finds what varies, and keys the pipeline on the parts that are state.

## N2, first: a stable key (2026-10-30)

Split into parts on a sample of the commits ("Native key parts"), the key's new values came from
the region after the depth target's pairs: `gt7_CommitDrawState`'s group 3 (16 viewports of
0x1c bytes at `+0x4cc..0x68c`), group 12 (screen scissor rectangles, `+0x69c..0x6b0`) and
group 16 (polygon offset values, `+0x6f0..0x718`), which change from draw to draw (cameras,
shadow cascades, decals) and which Vulkan sets as dynamic state. The key now leaves those out.
Race, 10 s windows of 300,000 to 355,000 marked draws:

| | |
|---|---|
| state key and material seen before | **99.7 to 99.9 %** |
| ... with other programs or another pipeline | **0.00 %** |
| key parts new, sampled | 0.0 to 0.1 % each (shaders up to 1.2 % in a scene change) |

A marked draw's pipeline and programs are a function of a key GT7's render thread makes in
0.3 us, and the keys repeat: the record's static half (pipeline, programs, material bindings)
can be built once per key and material.
