#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_STATEREPEAT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_STATEREPEAT_H_

#include <cstddef>
#include <cstdint>

// GT7 port, state repeats (KYTY_STATE_REPEAT=1, diagnostics): how often a draw's register state is
// the one of the draw before it, and whether what the renderer derives from that state (its
// programs, render targets and pipeline) repeats with it. GT7 commits a draw's state as prebuilt
// register blocks (SetCxRegistersIndirect and the like, from one engine function); the indirect
// context blocks each draw applies are recognised by address. A "State repeats" line every 10 s
// gives, for draws whose registers were all unchanged, only their context registers unchanged, or
// changed: how many, how often each derived result was the previous draw's, and the microseconds
// each step took: what reusing those results would save.
namespace Libs::Graphics::StateRepeat {

enum class Step : uint32_t {
	Programs,  // GetGraphicsPrograms
	Targets,   // ResolveRenderColorTarget / ResolveRenderDepthTarget
	Pipeline,  // TryGetGraphicsPipeline
	Rendering, // AcquireRenderTargets
	ImageSet,  // the texture cache's image set generation (no time): what reused targets need
	Count
};

[[nodiscard]] bool Enabled();

// GPU thread, command processor: an indirect context register block applied (its guest address
// and register count).
void NoteCxBlock(uint64_t address, uint32_t count);

// GPU thread, at the start of a draw's render state: its context registers, shader registers and
// user config registers, compared with the previous draw's (byte for byte).
void BeginDraw(const void* context, size_t context_size, const void* shaders, size_t shaders_size,
               const void* user_config, size_t user_config_size);

// GPU thread: a signature of a step's result for the current draw, and the cycles the step took.
void NoteStep(Step step, uint64_t signature, uint64_t cycles);

} // namespace Libs::Graphics::StateRepeat

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_STATEREPEAT_H_
