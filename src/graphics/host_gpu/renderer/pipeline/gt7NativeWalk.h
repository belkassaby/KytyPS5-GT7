#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_GT7NATIVEWALK_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_GT7NATIVEWALK_H_

#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::IR {
struct ResourceSnapshot;
} // namespace Libs::Graphics::ShaderRecompiler::IR

namespace Libs::Graphics::DrawPrep {
struct StageResult;
} // namespace Libs::Graphics::DrawPrep

// GT7 native backend N3 step 2 (docs/GT7-NATIVE-BACKEND.md): a marked draw's descriptor walk done
// on GT7's render thread, from the user data its record holds, with the program the GPU thread
// learned for the draw's state key, material and stage.
namespace Libs::Graphics::Gt7NativeWalk {

// A program source entry (PipelineCache's) and the walk's inputs besides the user data.
struct Learned {
	void*    entry           = nullptr;
	uint64_t shader_base     = 0;
	uint32_t user_data_count = 0;
	uint32_t stage           = 0; // the ShaderType
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

// N3 step 3, render thread: the stage's refresh as the draw-prep scanner prepares one
// (DrawPrep::StageResult: the entry, inputs, resources, specialization and its hash, and every
// byte read, in `reads`), for the GPU thread to take in place of its own walk (ProgramCache's
// TakeScanned, which checks the inputs and that the bytes are still there and GPU-clean).
// `stage`: the ShaderType. False when the walk fails.
[[nodiscard]] bool Prepare(const Learned& learned, const uint32_t* user_data, uint32_t stage,
                           DrawPrep::StageResult* result);

// N4, first piece: a record's pixel stage images on the render thread. The GPU thread teaches
// the pixel program's image resources (by the learned key) and the texture descriptions it made
// (by descriptor); the render thread finds, for each image of a prepared snapshot, the registered
// image a read-only texture cache lookup gives (TextureCache::LookupImage). `states`: 1 found
// (`ids` holds its index), 2 no description learned for the descriptor, 3 no registered image with
// that backing, 4 found as the GPU thread bound the same descriptor, with no image over its memory
// made or dropped since. The count, or 0 when the program's images are not learned.
uint32_t ResolveImages(uint64_t key, const ShaderRecompiler::IR::ResourceSnapshot& resources,
                       uint32_t* ids, uint8_t* states, uint32_t max);

// Both sides: a hash of a snapshot's buffers, images, samplers and flattened SRT.
[[nodiscard]] uint64_t Hash(const ShaderRecompiler::IR::ResourceSnapshot& snapshot);

} // namespace Libs::Graphics::Gt7NativeWalk

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_GT7NATIVEWALK_H_
