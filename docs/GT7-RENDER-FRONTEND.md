# GT7 render front-end: the draw path decoded (2026-10-29)

Follows [GT7-RENDER-STUDY.md](GT7-RENDER-STUDY.md), which found that GT7's draws go through seven
engine functions and one state-commit function. This reads those functions in the Ghidra decompile
(`GRANTURISMO7\work\ghidra-ps5`, eboot at `0x900000000`) and gives them types. Everything below is
applied to the Ghidra project by `ghidra-tools\GT7RenderTypes.java` from
`ghidra-tools\gt7-render-map.tsv`, which is the record of names, fields and evidence.

## The render context

Every draw takes the thread's **render context** from TLS (`fs:0` → `[-0xb8]`): a structure (at
least `0xe00` bytes, `GT7RenderContext`) that **shadows the GPU state** in the style of a D3D11
immediate context. A draw compares each argument with the shadowed value, and only on a change
writes it and sets a dirty bit. The parts decoded so far:

| Offset | Field | |
|---|---|---|
| `+0x08` / `+0x0c` | `dirty`, `dirty_enabled` | one bit per state group (below) |
| `+0x10` | `color_targets[8]` | 16 `CB_COLOR<n>_*` register pairs per render target |
| `+0x418` | `depth_target` | 17 pairs, `DB_Z_INFO` .. `DB_HTILE_DATA_BASE_HI` |
| `+0x718` / `+0x720` / `+0x724` | `index_size`, `index_offset`, `num_instances` | last values set |
| `+0x750` | `waits` | GPU waits to emit before the next draw |
| `+0x763` | `predicate` | draws are predicated (occlusion queries) |
| `+0x788` | `dcb` | the sceAgc command buffer (Kyty's `CommandBuffer`) plus register staging |
| `+0x770` | `attrib_slots` | vertex buffer slot per attribute semantic (a nibble each) |
| `+0x870` | `stages[6]` | per stage: shader, user-data SGPRs, constant buffer (below) |
| `+0xc94` | `prim_type` | |
| `+0xca0` | `vertex_buffers[16]` | the vertex buffer table (V#s), formats at `+0xda4` |
| `+0xde8` / `+0xdf0` | `transforms`, `bindings` | two transform sets (0x1240 bytes each) and which matrices go where |
| `+0xdf8` | `const_attribs` | constant vertex attributes |

## Committing state: register lists, not prebuilt blocks

`gt7_CommitDrawState` (`0x900016370`) walks the set bits of `dirty`. Each bit is a **state group**
that appends `{register, value}` pairs to **staging arrays in the command buffer**
(`gt7_StageCxRegisters` / `gt7_StageUcRegisters` allocate them downward from
`dcb.cursor_down`). At the end it emits one `Set{Sh,Cx,Uc}RegistersIndirect` per array. This
corrects the study: the indirect packets point at **lists the commit just built**, not at
prebuilt per-material blocks.

The register numbers come from globals that `gt7_InitRegisterTables` (`0x901633d30`) fills at
startup from `sceAgcGetRegisterDefaults2(13)`, looked up by hash. `agc_reg_defaults.cpp` replays
that lookup on Kyty's tables, and `agc_reg_tables_gt7.py` names all 203 globals
(`work\gt7-reg-tables.tsv`, `agc_<register>` in Ghidra). The state groups:

| Bit | Group | Registers |
|---|---|---|
| 0 | transforms | `gt7_BindTransform` per entry of `bindings` whose slot changed |
| 1 | colour targets | `CB_COLOR<n>_*` (stride `0xf` per target), defaults for unbound ones |
| 2 | depth target | `DB_Z_INFO`, `DB_STENCIL_INFO`, `DB_*_BASE`, `DB_HTILE_*`, `PA_SU_POLY_OFFSET_DB_FMT_CNTL` |
| 3 | viewport | `PA_CL_VPORT_*`, `PA_CL_VTE_CNTL`, `PA_SU_HARDWARE_SCREEN_OFFSET`, guard band |
| 4 | blend | `CB_BLEND<n>_CONTROL` |
| 5 | blend colour | `CB_BLEND_RED` .. |
| 6 | target mask | `CB_TARGET_MASK` |
| 7 | colour control | `CB_COLOR_CONTROL` |
| 8 | MSAA mode | `PA_SC_MODE_CNTL_0` |
| 9 | cull / face | `PA_SU_SC_MODE_CNTL` |
| 10 | alpha to mask | `DB_ALPHA_TO_MASK` |
| 11 | clipping | `PA_CL_CLIP_CNTL` |
| 12 | screen scissor | `PA_SC_SCREEN_SCISSOR_TL/BR` |
| 14 | depth/stencil? | context fields only, not decoded |
| 15 | texture gradients | `TEXTURE_GRADIENT_FACTORS/CONTROL` (Uc) |
| 16 | polygon offset | `PA_SU_POLY_OFFSET_FRONT/BACK_*` |
| 17 | GPU waits | `WaitRegMem` per entry of `waits`, then `AcquireMem` |

Then `gt7_WriteUserData` writes the shader user-data SGPRs of each enabled stage.

## The draws

- **`gt7_DrawIndex`** (`0x9000a1400`): `(prim_type, index_count, index_size, index_buffer,
  byte_offset, num_instances)` → set what changed, commit, `sceAgcDcbDrawIndex`.
- **`gt7_DrawAuto`** (`0x900018740`): `(prim_type, index_offset, vertex_count)` →
  `sceAgcDcbDrawIndexAuto`, one instance.
- **`gt7_DrawMeshClusters`** (`0x900005fb0`, the ~95 % one) is **not a single draw**: it draws a
  mesh split in **index clusters**. Per cluster it has an index count (`u16`) and 24 floats of
  bounds; it culls each cluster against a precomputed visibility bitset, or with the scene's
  culler (`PDIScene::Scene::Impl`, `PDIEXT::OcclusionCulling::OcclusionCullingCS`, with an inlined
  AVX bounds test), and issues **one `gt7_DrawIndex` per run of consecutive visible clusters**.
  Without clusters it makes one draw (`gt7_DcbDrawIndex`), which in a race is nearly every call
  (measured below).
  A second path (`alt_draw`, `0x902e7d460`) is not decoded.
- `0x900009bb0`, `0x90001a070`, `0x900053930`, `0x901a3a890`: not decoded yet.

## Measured: one draw per mesh (KYTY_DRAW_COUNTS=1, music rally, 2026-10-29)

`src/loader/gt7DrawCounts.cpp` counts the calls of `gt7_DrawMeshClusters` (and the clusters they
are given) and of `gt7_DrawIndex` (and those made from `gt7_DrawMeshClusters`). Over about two
minutes of a music rally race, per frame (10 s windows; the `Perf:` lines gave 3,300 to 4,500
draws a frame at 11 to 17 fps):

| | a frame |
|---|---|
| `gt7_DrawMeshClusters` calls | 2,000 to 3,500 |
| clusters they are given | 320 to 410 |
| `gt7_DrawIndex` calls from them | 1 to 3 |
| `gt7_DrawIndex` calls in all | 100 to 150 |

- **The cluster path is rare.** Most calls have no clusters (`cluster_count` 0) and take the
  single-draw path (`gt7_DcbDrawIndex` directly); the few clustered ones are almost all culled
  whole. In the menus it is the same (about 1,300 calls a frame, no cluster draws).
- So **`gt7_DrawMeshClusters` is one draw per call**, about 70 to 80 % of a race's draws, and the
  draw count is **not** a culling artefact: there are about 3,000 distinct mesh draws a frame
  (several passes: shadows, reflections, the main pass). Merging cluster runs would save nothing.

## What it means for a native path

- **The win has to come from the cost of each draw**, not from fewer draws: about 11 to 12 µs a
  draw on Kyty's one GPU thread today, where a native path that takes the engine's arguments and
  records Vulkan commands directly (on several threads) can be well under 1 µs.
- **The state is already flattened for us.** At the end of `gt7_CommitDrawState` the full state
  change of a draw is three plain register lists. Kyty can take them there, not by parsing PM4
  packets; and since GT7 only writes groups that changed, consecutive draws share most state.
- **Constants and vertex buffers are explicit** (next section): a native path gets each stage's
  constant buffer and the vertex buffer table as plain memory and descriptors, with the matrices
  that changed. Only textures and other shader resources are still unaccounted for.

## Per-draw inputs: constants, transforms, vertex buffers

**Stages.** `ctx->stages[6]` (`GT7StageBinding`, 0xb0 bytes each) holds per shader stage: the
**AGC shader header** it runs (`+0x0`; type byte at `+0x5a`), a shadow of its 32 **user-data
SGPRs** with dirty bits, and its **constant buffer**: a block of `constants_size` bytes in command
buffer memory whose descriptor goes in 4 SGPRs from `constants_slot`. `gt7_RenewStageConstants`
makes it copy on write: the first change in a draw moves the block to new command buffer memory
(copying it), so every draw's constants stay intact for the GPU. At the end of a commit,
`gt7_WriteUserData` writes the changed SGPRs of each stage (`gt7_WriteStageUserData`).

**Transforms (group 0).** What group 0 binds is not textures but **matrices**. `ctx->transforms`
points at two `GT7TransformSet`s (0x1240 bytes each); `ctx->bindings` lists up to 8 entries
`{u16 offset, u16 key}` (from the shaders): key bits 0-2 the stage, 3-6 a transform slot, 7
inverse, 8 transpose, 9 the second set, 10 as doubles. `gt7_BindTransform` copies the matrix
`gt7_GetTransform` returns into that stage's constants at `offset`. The 16 slots, in the style
of fixed-function transform state:

| Slot | |
|---|---|
| 0-3 | stored matrices (a small ring each); 0 and 1 behave as camera matrices, 3 as an object one |
| 4, 5 | slot 2 x slot 0 |
| 6 | a palette entry (instance or bone), or an inline matrix |
| 7, 8 | slot 0 x slot 6, slot 1 x slot 6 |
| 9 | derived from extra parameters (`+0x122c`..), not decoded |
| 10, 11, 12 | slot 0 x slot 9, slot 0 x slot 3, slot 1 x slot 3 |
| 15 | identity |

Derived matrices and inverses are computed when first asked for after a change (dirty bits in
`+0x1220`). Names (view, projection, world) are not confirmed; for DLSS later, the camera slots
and whatever keeps the previous frame's matrices are what motion vectors need.

**Vertex buffers.** `ctx->vertex_buffers[16]` is the table the vertex shader fetches from: 16-byte
V#s, with a fetch format per slot at `+0xda4` (`format << 5 | slot`). Meshes put their buffers in
(for example `gt7_Draw_9bb0`: address, stride `0x1c`, count, in the slot `attrib_slots` gives each
attribute), and `gt7_PrepareDrawBuffers` adds **constant attributes** (`ctx->const_attribs`, 16
values up to 16 bytes with a type: 1-4 floats or ints, packed formats) as stride-0 buffers in frame
memory. `gt7_WriteUserData` copies the table and the format list into command buffer memory and
puts their addresses in the user-data SGPRs the vertex shader declares (its header's `+0x10` and
`+0x14`).

## Textures: from the package record to the shader, per draw

A model is drawn by `gt7_DrawModel_1440` / `gt7_DrawModel_72010`: for each part,
**`gt7_BindMaterial`** (`0x900011800`), then `gt7_DrawMeshClusters`. For each shader stage of
the material (an entry of 0x40 bytes in the shader set: the stage, its shader, the material
constants' size and buffer slots, and lists of texture and sampler bindings), the bind:

- copies the material's **constants** into new memory, patches per-instance values in, and binds
  the block as a buffer (`gt7_BindBuffer`);
- for each **texture binding** `{texture index, resource index}`: `gt7_BindTexture(&package->
  textures[material->textures[i]].tsharp, stage, index)`;
- for each **sampler binding**: `gt7_BindSampler(&...textures[j].ssharp, ...)`.

`package->textures` are the **0x48-byte texture records** of the texture packages
(`GT7TextureRecord`: the prebuilt T#, then the texture's S#, the chunk index at `+0x36`) that
`gt7_RelocateTexturePackage` (`0x900688100`) fixes up at load. The binders copy the T# (8 dwords),
S# or V# (4 dwords) **straight into the stage's user-data SGPR shadow**, at the register the
shader's header declares for that resource (`gt7_ShaderSharpSgpr`: the AGC `sharp_resource_offset`
tables, class 0 textures, 2 samplers, 3 buffers). The commit then writes the changed SGPRs. A
GT7 draw has **no resource table in memory**: its textures are user-data registers, copied from
records whose addresses are stable for the life of a package.

Two more details from the same code:

- `gt7_DrawMeshClusters`'s arguments 14 and 15 (were "stats") are a user-data SGPR of stage 3 (from
  its shader resource table slot) and an index count per primitive: per run it writes the run's
  first **primitive ID** there, for the pixel shader. So stage 3 is the pixel stage.
- The 59 functions behind `gt7_SetShUserData` (`0x9016xxxxx`) are **compute passes** (a global
  shader each, then a dispatch); likewise `0x900188ad0`, which the study took for a per-draw
  texture record builder, is the `CopyImageCube_CS` pass (`gt7_DispatchCopyImageCube`: its 88-byte
  record is that shader's resource table). Compute passes find their shaders by name
  (`gt7_FindShader`).

**Measured: that path is rare (KYTY_TEXTURE_COUNTS=1, music rally, 2026-10-29).**
`src/loader/gt7TextureCounts.cpp` records every package at `gt7_RelocateTexturePackage` (611
packages, 7,267 textures by the race) and looks up each `gt7_BindTexture` T# among their
records. In the race `gt7_BindTexture` runs **1 to 2 times a frame** (against 3,300 to 3,700
draws), always for one texture outside the packages (`0x1225670000`). The texture-list part of
`gt7_BindMaterial` is almost never taken, so material textures reach the shaders another way.
The 67 functions with inlined T# binds into user data are mostly post-process and compute
binders (`0x9000edf60`: a T# into a pass's own stage binding, by pass number).

**Likely instead: in the material constants.** `gt7_BindMaterial` copies each material's
constant block (`material + 0x18`) into new memory every draw (`FUN_9000665f0`), patches
per-instance values in and binds it as a buffer. If those blocks hold the T#s, the pixel shader
loads them from that buffer, which fits the study's T#s in per-frame tables (the copies) and in
CPU-side material objects (the sources).

**Measured: material textures live in the material constant blocks.** Hooks on
`gt7_BindBuffer` (1 call a frame) and `FUN_9000665f0` (none) found nothing, so the next hook
scanned, at every `gt7_CommitDrawState`, each stage's user-data SGPRs and the memory behind SGPR
pairs that hold a guest pointer. Per frame in a race (2,400 to 4,600 commits):

| Stage | package T#s in SGPRs | behind pointers |
|---|---|---|
| 0 (vertex) | 0 | 500 to 1,400 |
| 1 (pixel) | 85 to 97 | **15,000 to 27,000** (about 7 a draw), behind **SGPRs 2-3**, at offsets 0x0, 0x20, 0x40 ... |
| 2 to 5 | 0 | a few dozen (stage 5, compute, none) |

About 1,150 to 1,450 distinct package textures in 10 s. (So the pixel stage is stage 1, not 3.)
A hardware write watch on a context's stage-1 SGPRs 2-3 and on the table's first T#
(`Common::GuestWatch`) named the writer: libc `memcpy`, called from `0x900012cff` (and a copy at
`0x900012332`), in **`gt7_BindMaterial`**. There it allocates space inline from the command
buffer (`cursor_down`), copies the material's constant block (`material + 0x18`, in the
`0xec07...` material objects; 0xcc to 0x154 bytes) with `memcpy(dst, block, stage_entry->size)`,
patches per-instance values in, and puts the copy's address in the pixel stage's SGPRs 2-3.

**The path, all of it:** package file → `gt7_RelocateTexturePackage` (T# base filled in) →
**material constant blocks** in the material objects hold copies of those T#s (with the
constants) → per draw `gt7_BindMaterial` copies the block into command buffer memory → its
address in SGPRs 2-3 → the pixel shader loads the T#s from it. (The render context is not a
fixed per-thread object: the address the hook saw lies in per-frame memory, `0x1000...`.)

**For step 4:** a material's block keeps its T#s at fixed offsets, so a native path can map each
block offset to its package record once per material (by the 8-byte T# head) and then, per draw,
take the material pointer and bind bindless indices instead of reading T#s from GPU memory.

**Who fills the blocks: `gt7_ResolveMaterialTextures` (`0x900689e80`), at load.** A hook at the
block copy in `gt7_BindMaterial` (`0x900012cea`; 22 to 25 copies a frame, each holding package
T#s) kept four material blocks' T# slots; value-filtered watches on them (`ArmExpecting`) caught,
while the next race loaded, `0x900689f57` writing them, called from
`GTStage::LoaderUtil::ModelSetHolder`. For each material of a set (`GT7Material`, 0x30 bytes at
`set + 0x48`) and its shader entry (`GT7MaterialShader`, 0x28 bytes at `set + 0x50`) it copies:

- each texture's T#, `package->textures[material->texture_indices[i]].tsharp` (32 bytes), into the
  block at the shader's slot offset, or GT7's **default texture** (`0x906316388`, in `.bss`) for
  an index of -1 or an empty record;
- each sampler's S# (16 bytes, from 0x18-byte entries at `package + 0x48`);

then builds the block's V# into `material->block_desc`, which `gt7_BindMaterial` binds as it is
when the material has no per-instance patches.

**So, end to end:** package file → `gt7_RelocateTexturePackage` (T# bases) →
`gt7_ResolveMaterialTextures` (T#s and S#s into each material's constant block, once a load) →
per draw `gt7_BindMaterial` (the block, or a patched copy of it, behind the pixel stage's SGPRs
2-3) → the pixel shader. **For step 4** a native path hooks `gt7_ResolveMaterialTextures`: there
it gets, for every material, which package record (or the default texture) sits at which block
offset, and can give the material a table of bindless indices once, at load.

## Next

1. The remaining four draws and group 14.
2. Prototype step 4: register textures at `gt7_RelocateTexturePackage` and materials at
   `gt7_ResolveMaterialTextures` in Kyty, and at draw time look a block up by address instead of
   decoding its T#s.
3. Prototype: hook `gt7_DrawMeshClusters` in Kyty and record its arguments and the committed
   register lists, before replacing anything.
