#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWMUTATIONS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWMUTATIONS_H_

#include <cstdint>

// GT7 port, native path milestone 4, measured first (docs/GT7-STEP5-PLAN.md): what a draw changed
// in the renderer's shared state. A draw that changes none of it (its images, buffers and
// pipeline exist, nothing is uploaded, no barrier, the render pass is the open one) could be
// recorded on another thread. The renderer marks each kind where it happens, on the GPU thread;
// loader/gt7NativeDraws reads and clears the marks around each draw. Marking is an OR into a
// thread-local word.
namespace Libs::Graphics::DrawMutations {

enum Bit : uint32_t {
	Barrier      = 1u << 0u, // an image layout or access barrier recorded (Image::Transit)
	RenderPass   = 1u << 1u, // a render pass begun or ended
	NewImage     = 1u << 2u, // an image created in the texture cache
	ImageUpload  = 1u << 3u, // image data uploaded
	NewBuffer    = 1u << 4u, // a buffer created in the buffer cache
	BufferUpload = 1u << 5u, // buffer data uploaded
	NewPipeline  = 1u << 6u, // a graphics pipeline created
	Count        = 7,
};

inline thread_local uint32_t t_mask = 0;

inline void Note(Bit bit) {
	t_mask |= bit;
}

} // namespace Libs::Graphics::DrawMutations

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWMUTATIONS_H_
