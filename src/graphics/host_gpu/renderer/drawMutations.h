#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWMUTATIONS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWMUTATIONS_H_

#include <array>
#include <cstdint>

// GT7 port, native path milestone 4 (docs/GT7-NATIVE-PATH.md): what a draw writes in the
// renderer's shared state, for recording draws on other threads. A draw that writes none of it
// could be recorded by a worker without locks or rollback. The renderer marks each kind where it
// happens, on the GPU thread, in the draw's phase: prepare (its programs, bindings, vertex buffers
// and pipeline lookup) or record (render targets onward; renderDraw.cpp switches the phase).
// loader/gt7NativeDraws resets the marks before each draw and reads them after it. Marking is an
// OR into a thread-local word.
namespace Libs::Graphics::DrawMutations {

enum Bit : uint32_t {
	Barrier      = 1u << 0u,  // an image layout or access barrier recorded (Image::Transit)
	RenderPass   = 1u << 1u,  // a render pass begun or ended
	NewImage     = 1u << 2u,  // an image created in the texture cache
	ImageUpload  = 1u << 3u,  // image data uploaded
	NewBuffer    = 1u << 4u,  // a buffer created in the buffer cache
	BufferUpload = 1u << 5u,  // buffer data uploaded
	NewPipeline  = 1u << 6u,  // a graphics pipeline created
	ProgramEntry = 1u << 7u,  // a program cache entry's resources replaced (walk, prepared take)
	TextureDesc  = 1u << 8u,  // a texture descriptor added to the descriptor cache
	TextureMemo  = 1u << 9u,  // a texture memo entry written
	NewSampler   = 1u << 10u, // a sampler created
	Count        = 11,
};

inline thread_local uint32_t                t_phase = 0; // 0 prepare, 1 record
inline thread_local std::array<uint32_t, 2> t_masks {};

inline void Note(Bit bit) {
	t_masks[t_phase] |= bit;
}

inline void Reset() {
	t_phase = 0;
	t_masks = {};
}

} // namespace Libs::Graphics::DrawMutations

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWMUTATIONS_H_
