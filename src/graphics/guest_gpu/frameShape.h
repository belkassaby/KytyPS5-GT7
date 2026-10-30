#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_FRAMESHAPE_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_FRAMESHAPE_H_

#include <cstdint>

// GT7 parallel renderer, step 1 (docs/GT7-PARALLEL-RENDERER.md): the ceilings.
//
// KYTY_FRAME_SHAPE=1: the shape of the guest's GPU work, counted by the command processors (the
// GPU thread), a "Frame shape:" line every 10 s: submissions and indirect buffers a frame by
// queue type; draws and dispatches a submission and a run of one render target; and how many
// draws and dispatches take key state (programs, render targets, viewport) from registers no
// packet of their submission or indirect buffer wrote, i.e. what decoding command buffers apart
// would have to carry over.
//
// KYTY_NULL_RENDER=1: draws and dispatches are skipped (the renderer is not called); everything
// else (labels, end-of-pipe writes, DMA, waits, flips) runs. The frame rate is then GT7's own CPU
// side with Kyty's translation cost near zero. The picture is wrong by design.
namespace Libs::Graphics::FrameShape {

[[nodiscard]] bool Enabled();
[[nodiscard]] bool NullRender();

// A submission's first slice begins (`graphics`: a graphics queue's, else compute).
void OnSubmission(const void* processor, bool graphics, uint32_t frame);
// An indirect buffer is entered (`chain`: a chained one, which replaces the current buffer).
void OnIndirectBuffer(const void* processor, bool chain);
// Register writes, by normalized offset.
void NoteCx(const void* processor, uint32_t reg, uint32_t value);
void NoteSh(const void* processor, uint32_t reg, uint32_t value);
void OnDraw(const void* processor);
void OnDispatch(const void* processor);
// Parallel execution slice 1, measured first: every packet the GPU thread's command processor
// runs. Packets with no effect besides register state, draws and dispatches (register writes,
// index and instance state, indirect buffers, markers) extend a chunk; any other packet (labels,
// waits, DMA, event writes, cache actions) ends it. A "Chunks:" line every 10 s: chunks a frame,
// commands by their chunk's length, and the packets that end chunks.
void OnPacket(const void* processor, uint32_t opcode, uint32_t custom);
// The packet classes OnPacket and the replay (KYTY_REPLAY) use: one that only sets state, draws or
// dispatches; and of those, a draw, a dispatch or the GT7 identity marker (what the replay runs
// and the main processor skips).
[[nodiscard]] bool IsPurePacket(uint32_t opcode, uint32_t custom);
[[nodiscard]] bool IsCommandPacket(uint32_t opcode, uint32_t custom);
// Slice 3 (KYTY_REPLAY_SOFT=1): packets with effects the replay can run in order inside a chunk,
// recorded into the chunk's commands: end-of-pipe releases (labels, cache actions) and event
// writes (barriers, flushes). Not WRITE_DATA, which can also write registers.
[[nodiscard]] bool IsSoftPacket(uint32_t opcode, uint32_t custom);

} // namespace Libs::Graphics::FrameShape

#endif // EMULATOR_SRC_GRAPHICS_GUEST_GPU_FRAMESHAPE_H_
