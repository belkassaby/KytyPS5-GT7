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

## Milestone 3: draw templates

Per (material, mesh, pixel and vertex programs, pass state): the pipeline, the vertex buffer
layout and the bindings, with only the per-draw parts (constant buffer addresses, index range,
primitive ID base, transforms) filled in at the draw. Measure first how stable the keys are frame
to frame and how much of a draw's 10 us a template removes.

## Milestone 4: recording on worker threads

Templates make a marked draw self-contained: runs of them in one render pass can be recorded into
secondary command buffers by worker threads while the GPU thread keeps everything that mutates
(GT7-STEP5-PLAN.md), which is where the frame rate gains past one thread's limit come from.
