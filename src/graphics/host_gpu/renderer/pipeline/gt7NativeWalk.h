#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_GT7NATIVEWALK_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_GT7NATIVEWALK_H_

#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::IR {
struct ResourceSnapshot;
} // namespace Libs::Graphics::ShaderRecompiler::IR

// GT7 native backend N3 step 2 (docs/GT7-NATIVE-BACKEND.md): a marked draw's descriptor walk done
// on GT7's render thread, from the user data its record holds, with the program the GPU thread
// learned for the draw's state key, material and stage.
namespace Libs::Graphics::Gt7NativeWalk {

// A program source entry (PipelineCache's) and the walk's inputs besides the user data.
struct Learned {
	void*    entry           = nullptr;
	uint64_t shader_base     = 0;
	uint32_t user_data_count = 0;
};

// Render thread: walks the entry's resource plan with `user_data` (user_data_count of them),
// reading only bytes no GPU work has written; the snapshot's hash, or false (unreadable).
[[nodiscard]] bool Walk(const Learned& learned, const uint32_t* user_data, uint64_t* hash);

// Why this thread's last Walk failed: 1 more user data than a record holds, 2 a read refused
// (its address and size in *address, *size), 3 the walk failed otherwise.
[[nodiscard]] uint32_t LastFailure(uint64_t* address, uint32_t* size);

// Totals of the walks' reads: count, nanoseconds in all, of those in the refused probes, and
// reads served from the backing store after the probe refused them.
void ReadStats(uint64_t* reads, uint64_t* read_ns, uint64_t* probe_ns, uint64_t* fallbacks);

// Both sides: a hash of a snapshot's buffers, images, samplers and flattened SRT.
[[nodiscard]] uint64_t Hash(const ShaderRecompiler::IR::ResourceSnapshot& snapshot);

} // namespace Libs::Graphics::Gt7NativeWalk

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_GT7NATIVEWALK_H_
