#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERMAPPING_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERMAPPING_H_

#include <cstdint>
#include <span>

namespace Libs::Graphics::ShaderRecompiler::IR {
struct DescriptorValue;
} // namespace Libs::Graphics::ShaderRecompiler::IR

// GT7 port, render mapping (KYTY_MAP_TEXTURES=1, diagnostics): finds where the game keeps the
// texture descriptors (T#s) its draws use. The GPU thread counts the non-bindless T#s it binds;
// once frames have been heavy (2,000 bindings or more) for 15 s, the busiest T#s are searched for
// (mipmapped ones: render targets have one level) in all of the guest's readable memory, each
// place is logged ("Texture map:"), and up to four
// places get a value-filtered write watch (Common::GuestWatch::ArmExpecting), so the code that
// writes those descriptors (texture creation, streaming, the per-frame tables) is logged with its
// callers.
namespace Libs::Graphics::RenderMapping {

[[nodiscard]] bool Enabled();
// GPU thread: the T#s one draw or dispatch stage binds (`bindless`: which of them are bindless).
void NoteTextures(std::span<const ShaderRecompiler::IR::DescriptorValue> images,
                  std::span<const uint8_t> bindless, uint64_t frame);

} // namespace Libs::Graphics::RenderMapping

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERMAPPING_H_
