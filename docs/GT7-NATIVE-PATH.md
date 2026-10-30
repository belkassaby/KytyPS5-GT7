# GT7 native path (2026-10-29)

Kyty translates GT7's AMD command buffers draw by draw, rediscovering every draw's resources from
registers and memory (about 10 us a draw on one thread, GT7-STEP5-PLAN.md). The native path gives
it GT7's own description of each draw instead, from the engine functions decoded in
GT7-RENDER-FRONTEND.md, so the work can be keyed on stable engine objects (materials, meshes,
shaders) and done once, not per draw.

**The constraint:** GT7 builds command buffers on several threads and submits them later in a
fixed order, with render-target changes, ~1,300 compute passes a frame and barriers in between,
all still executed by Kyty's command processor. A native draw cannot be issued when the engine
calls its draw function; it has to happen where the draw packet is in the stream.

## Milestone 1: the identity channel (done)

`src/loader/gt7NativeDraws.cpp` (`KYTY_GT7_NATIVE=1`):

- Hooks on `gt7_BindMaterial` and `gt7_DrawMeshClusters` keep, per game thread, the material set,
  material, the material's constant block, the shader set and the mesh.
- Kyty's `AgcDcbDrawIndex` (libs/agc.cpp), when GT7's call returns into `gt7_DrawIndex` or
  `gt7_DrawMeshClusters` (the draw wrappers tail-jump, so the return address is the engine's),
  writes a 12-dword `IT_NOP` with custom code `R_GT7_DRAW` (0x1B) carrying them, just before the
  draw packet.
- The command processor (`CpOpGt7Draw`) holds the identity for the next indexed draw and checks it
  against the pixel stage's user data.

Measured in a music rally race, per frame:

| | |
|---|---|
| indexed draws | 2,100 to 4,300 |
| marked with an identity | **76 to 88 %** (1,800 to 3,600) |
| markers written = seen, none without its draw | yes (in order) |
| pixel stage's texture table (SGPRs 2-3) = the material's block | 65 to 81 % of the marked |
| material without a block | ~300 |

The rest of the marked draws have a per-draw copy of the block (patched materials) or a pixel
shader that uses SGPRs 2-3 otherwise. The game runs unchanged with the markers on.

## Milestone 2: pixel-stage bindings per material

For a marked draw whose pixel-stage table is the material's block, the textures and samplers the
pixel shader binds are the material's (filled once at load by `gt7_ResolveMaterialTextures`, and
the blocks do not change afterwards except a handful with animated constants). The renderer keeps,
per (material, pixel program), the bindings `PrepareBindings` made, and reuses them while the
block's T#s and S#s, the texture cache's image set and the images' memory stamps are unchanged;
otherwise it prepares them as now. A verify mode compares both. Expected: part of the `textures`
phase (1.7 us a draw), a few percent; the point is the mechanism, keyed on engine identity.

**Done** (`PrepareBindings`, `KYTY_GT7_NATIVE_BINDINGS=0` off, `KYTY_GT7_NATIVE_VERIFY=1`):

- Almost every GT7 material shader binds a **light grid** (clustered lighting), which has
  per-draw checks (`CheckLightGridView`): first made whole entries uncacheable (0.1 to 1.2 %
  reused); now the light grids are resolved every draw and not compared, and the rest is reused.
- Verify run: **62 to 72 % of the material draws reused, 0 mismatches**.
- A/B, music rally race windows:

| | off | on |
|---|---|---|
| pixel bindings of a material draw | 3.00 us | **2.07 us** |
| `textures` phase a draw | 2.02 us | **1.62 us** |
| a draw | 11.00 us | **10.55 us** |
| draws a frame / fps | 3,441 / 15.9 | 3,645 / 16.1 |

About 4 % of a draw: as expected, the mechanism, not the frame rate.

## Milestone 3: draw templates

Per (material, mesh, pixel and vertex programs, pass state): the pipeline, the vertex buffer
layout and the bindings, with only the per-draw parts (constant buffer addresses, index range,
primitive ID base, transforms) filled in at the draw. Measure first how stable the keys are frame
to frame and how much of a draw's 10 us a template removes.

### Milestone 3, measured: templates would hit, but save little each

- Template keys (material, mesh, programs, pipeline) of marked draws: **99.6 to 100 % seen before**
  (53 to 74 % the frame before, 14 to 25 % earlier in the same frame, 9 to 22 % older).
- Marked draws cost 10.0 to 13.5 us in the renderer, the others 7.1 to 8.8 us.
- But a draw's cost is spread thin, and the per-draw inputs (copy-on-write constants, transforms,
  index range, targets) keep most of it per draw: `CommitBindings` is 1.05 to 1.55 us (image
  transitions 0.35 to 0.76, recording 0.33, descriptor writes 0.23, spec checks 0.15, bindless
  0.06), so reusing descriptor sets would save ~0.4 us. Templates as a whole: ~2 us of ~11.
  Parallelism is the multiplier; milestone 4 went first.

## Milestone 4: recording on worker threads

Templates make a marked draw self-contained: runs of them in one render pass can be recorded into
secondary command buffers by worker threads while the GPU thread keeps everything that mutates
(GT7-STEP5-PLAN.md), which is where the frame rate gains past one thread's limit come from.

**What a draw writes in shared state** (`DrawMutations`, `renderer/drawMutations.h`): the renderer
marks, per draw and phase (prepare: programs, bindings, vertex buffers, pipeline lookup; record:
render targets onward), barriers, render pass changes, new images and buffers, image and buffer
uploads, new pipelines, program cache entries replaced, texture descriptors added, texture memos
written and samplers created. Music rally race, marked draws:

| | first audit | after per-draw snapshots |
|---|---|---|
| nothing written while preparing | 0.0 to 0.1 % | **97.8 to 98.8 %** |
| nothing written while recording | 92.5 to 93.6 % | 92.4 to 93.4 % |
| nothing written at all | 0.0 to 0.1 % | **91.9 to 93.0 %** |
| program entry replaced while preparing | 99.8 to 100 % | 0.1 to 0.7 % (full walks) |

Recording: render pass changes 6.4 to 7.6 %, barriers 2.4 to 3.3 %. Preparing: each other kind
under 1 %. Clean draws take ~10 us in the renderer, the others 33 to 66 us.

**Per-draw snapshots** (commit 84ecd733): the program cache kept each draw's refresh of a stage
(resource snapshot, specialization) in the program's shared entry, written by every draw of the
program. It is now kept per thread and stage (`ProgramCache::StageSnapshot`), and the draw's
runtime points there. `KYTY_DRAW_PREP_VERIFY=1` over a race: 15.9 million prepared refreshes
used, 0 differing from a walk. A/B: 15.9 fps against 15.7 (a draw 11.51 us against 11.19, other
scenes: within noise).

**Next: prepare-ahead.** While the GPU thread records draw N, a worker prepares the next marked
draws from register state the draw-prep scanner keeps; the GPU thread takes a prepared draw when
its inputs still match and prepares it itself otherwise (and when preparing it would write shared
state), with a verify mode. Then recording on workers (secondary command buffers).

### Prepare-ahead stage 2, measured: where a pixel stage's texture resolve goes

`Native resolve:` (commit 9a5b9308), music rally race: a marked draw's pixel stage binds 10.8
images in about 2.0 us: key and lookup 0.50 us (what a worker could take as is), memo checks
0.40 us for 7.3 hits, slow path 0.5 to 0.9 us for 3.5 images, BindImage 0.28 us.

`Native slow path:` (commit 13e64088) says what the slow path is: **73 % depth-compare bindings**
(shadow maps: a 1x1 placeholder, a 2048x2048 depth, two 8704x8704 cascades, all at fixed
addresses) and **27 % light grids** (30x17, a new per-frame address every draw). Stale memos:
0.3 %. The memo mechanics are fine; it is these two kinds.

Making depth-compare bindings memo-eligible (tried 2026-10-30, not kept) gave 0 memo hits: the
shadow maps' T#s never find their depth image by the same backing (`TextureCache::FindImage`);
each lookup goes through overlap resolution, which the memo leaves out by design (an alias can
age out with nothing registered). Keeping them would need the overlap result cached with its own
validity. Per GT7-PARALLEL-RENDERER.md, the small per-draw wins stop here: the work moves to the
parallel renderer, where these lookups belong to the serial resolve stage, keyed by identity.
