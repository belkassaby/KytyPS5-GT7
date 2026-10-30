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

## N3, first: stage constant blocks are rare (2026-10-30)

`KYTY_GT7_NATIVE_RECORDS=1`: at each marker (after the commit) the render thread copies the
vertex and pixel stages' constant blocks (`GT7StageBinding::constants`, `constants_size`) into a
per-thread ring and the marker carries the record; the GPU thread checks at the draw that the
stage's descriptor at `constants_slot` points at the block and that guest memory still holds the
copy. Race, 10 s windows of 275,000 to 350,000 marked draws:

| | |
|---|---|
| records made on the render threads | 0.06 to 0.07 us each; 0 stale on the GPU thread |
| stages with a constant block | **only 13,000 to 17,000** (~5 % of the draws' stages; 10 to 13 bytes a record on average) |
| of those, descriptor at the block / bytes as copied | 78 to 83 % / the same (the rest: slot past SGPR 31, `user_data_extra`) |
| frame rate | 16.6 fps median, as without records |

The stage constant blocks are the exception, not the per-draw data. A draw's inputs reach its
shaders through its **user-data SGPRs** (the engine's shadow at `GT7StageBinding::user_data`,
`+0x18`, 32 of them, and `user_data_extra` past them): the material's block copy (pixel SGPRs 2-3),
transforms, and the pointers the shaders' descriptor walks follow. So the record's per-draw half
is the stages' user data, and what their pointers lead to: the descriptor walk Kyty's scanner
does today, done on the render thread from the engine's shadow, with each program's resource plan
(known after its first translation).

## N3 step 1, measured: the engine's user-data shadow is not the SGPRs (2026-10-30)

Records with the vertex and pixel stages' SGPRs 0-31 from the engine's shadow
(`GT7StageBinding::user_data`), compared at the draw with Kyty's: only **27 to 38 %** of stages
match on every SGPR (race, 580,000 to 700,000 stages per 10 s). Vertex SGPRs 10-11 differ in
about two thirds of the vertex stages; pixel SGPRs 8 to 28 in most pixel stages. A sample of the
values:

- the shadow holds **0** where Kyty has a sampler (pixel 12-15 and 20-23: `0x00000005 0x00700000
  0x00280000 0x00124061`, an S#), or a buffer descriptor (vertex 12-19, pixel 16-17), or the
  vertex buffer table pointer (vertex 10-11);
- or it holds an **older value** (pixel 0-1: a pointer `0xc8200000_125c6000` against Kyty's
  `0x10_00548800`; pixel 18-19: another S# than the one bound).

GT7 writes these SGPRs into the command buffer **directly**, not through the shadow: the
material's textures and samplers (`gt7_BindTexture` / `gt7_BindSampler` from `gt7_BindMaterial`,
the slot from the shader's resource offsets), the vertex buffer table, buffer descriptors. The
shadow only tracks what `gt7_SetUserData` writes. So the record's user data has to be assembled
from GT7's **bind calls** as they write, per stage and SGPR: hooks on the writers the decompile
names, building the render thread's own complete copy, checked against Kyty's SGPRs until it
matches every draw.

## N3 step 1, done: the engine's user data is exactly the draw's (2026-10-30)

The mismatches were the comparison's: Kyty keeps, per stage, the highest SGPR ever written (with
the stale values of other shaders above the current one's), while the hardware loads only the
user SGPRs the shader declares (`RSRC2.USER_SGPR`). Compared on those, picking each record's
stages by shader type (the header's byte at `+0x5a`: GT7 stage 1 is the PS, stage 0 the GS,
stage 4 a GS in a few draws), race, 10 s windows of 610,000 to 720,000 stages (~7 SGPRs each):

**100.00 % of stages, every SGPR as the engine's shadow, 0 differing.**

Every SGPR writer found goes through the shadow (`gt7_BindTexture`, `gt7_BindSampler`, the
material block, the constants' descriptor), and `gt7_WriteStageUserData` writes the changed ones
from it. A GS-back or HS-back shader (types 6, 7; none among the marked draws) takes its 32
through memory, with SGPRs 0-1 pointing at them (`gt7_SetBackUserDataPointer`).

So the render thread holds, at each marker, the exact per-draw inputs of the vertex and pixel
stages, in engine terms (which SGPR is which texture, sampler, block, table is known from the
binds). Next, N3 step 2: the descriptor walk on the render thread from this user data, with each
program's resource plan, checked against Kyty's walk of the same draw.

## N3 step 2, done: the descriptor walk on GT7's render threads (2026-10-30)

`KYTY_GT7_NATIVE_RECORDS=walk`. The GPU thread, after a marked draw's vertex or pixel stage
refresh, teaches the render threads the program it used for the draw's state key, material and
stage (`Gt7NativeWalk::Learned`: the source entry, shader base, declared user data count), and
hashes its resource snapshot (buffers, images, samplers, flattened SRT). At the next markers of
the same key and material the render thread walks that program's resource plan from the record's
user data (`PipelineCache::Gt7Walk`) and the GPU thread compares the hashes.

Getting the walk's cost down took four runs, each measured:

| render thread walk | us a stage | frame rate |
|---|---|---|
| clean-backing reader only (`TryReadBufferBacking`: refuses GPU-visible memory off the GPU thread) | 1.3, 98 % unreadable | 16 |
| guest mapping fallback (page faults on GT7's threads) | ~130 | 6 to 8 |
| backing store (`TryReadBacking`: the address space's commit lock on each of ~130 reads) | ~120 | 8 to 9 |
| backing pointer through the mapping cache (`FindBacking`) | ~57 | 14 |
| the walker's page fast path (`map_clean_page` -> `FindBacking`), each thread its own plan copy (`ScannerPlan`) | **3.8 to 3.9** | **15.9** median |

Race, 10 s windows of 530,000 to 550,000 stages: **97.4 to 98.7 % the same resources as the GPU
thread's walk, 0.00 % different**, the rest stages whose program was not learned yet. A record
(both stages, user data, walks) costs 6.6 us on the render thread. The frame rate is as without
records: GT7's render threads have the time.

Open: the walk reads the backing store without asking whether GPU work wrote the bytes (off the
GPU thread nothing can say so); the bytes it reads are mostly the render thread's own command
buffer memory. Before a record replaces the GPU thread's walk, its reads are checked as the
draw-prep scanner's are (the bytes compared at the draw), or confined to memory the engine knows
the CPU wrote.

## N3 step 3, done: the GPU thread uses the render threads' walks (2026-10-30)

`KYTY_GT7_NATIVE_RECORDS=use`: the render thread prepares each marked draw's vertex and pixel
stage as the draw-prep scanner does (`PipelineCache::Gt7Prepare`: a `DrawPrep::StageResult` with
the program entry, inputs, resources, specialization and its hash, and every byte read recorded
in its `ReadSet`, served through `FindBacking`'s mapping cache). The GPU thread's
`ProgramCache::Get` takes the record's stage before the scanner's and checks it as it checks the
scanner's (`TakeScanned`: same entry, shader base and user data; every recorded byte still there
and GPU-clean, `ReadSet::Validate`), which closes step 2's open point: a byte GPU work wrote since
the render thread read it fails the check and the stage is walked on the GPU thread.

Race with `KYTY_DRAW_PREP_VERIFY=1` (every result taken walked again on the GPU thread and
compared), 10 s windows: 324,000 to 365,000 prepared stages reached their draw, **100.0 % taken
and used in place of the GPU thread's walk**; the draw-prep line: 752,297 checks (records' and
scanner's), **0 mismatches**; no crash.

What it changes and what not: the scanner prepared these walks ahead already, so the GPU thread's
work per stage is the same (take, check the bytes, apply). The point is the source: each marked
draw's shader resources now come from GT7's render thread, from engine state, checked at the
draw, which is what N4's records need.

## N4, first piece: a record's textures found on the render thread (2026-10-30)

With `KYTY_GT7_NATIVE_RECORDS=use`: a marked draw's pixel stage teaches the render threads its
program's image resources (by state key and material) and the texture descriptions it made (by
descriptor, `MakeTextureDescKey`); the render thread, for each image of its prepared snapshot,
finds the description and asks the texture cache for the registered image with that backing
(`TextureCache::LookupImage`: read-only, under the cache's lock, FindImage's first step only);
the GPU thread compares with the image each binding got. Race, 10 s windows of 235,000 to
260,000 marked pixel stages, 2.5 to 2.9 million images:

| | |
|---|---|
| the image the GPU thread bound | **77.6 to 78.5 %** |
| another image | **0.00 %** |
| no description learned for the descriptor | 6.7 to 7.1 % (descriptors new to the frame: the light grids, at a new address each draw) |
| no registered image with that backing | 14.6 to 15.7 % (images the GPU thread finds through overlap resolution, the shadow maps among them, or makes at the draw) |
| frame rate | 15.9 median, as without |

Never a wrong image. The 22 % the render thread cannot name are the cases the texture
investigations found (light grids, shadow maps): they stay with the GPU thread, per image.
