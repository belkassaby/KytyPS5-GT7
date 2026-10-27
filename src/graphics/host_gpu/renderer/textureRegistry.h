#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTUREREGISTRY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTUREREGISTRY_H_

#include <cstdint>
#include <span>
#include <string>

namespace Libs::Graphics::ShaderRecompiler::IR {
struct DescriptorValue;
} // namespace Libs::Graphics::ShaderRecompiler::IR

// GT7 port, texture fast path, stage 1 (KYTY_TEXTURE_REGISTRY=1): a registry of the textures GT7
// loads from its texture packages. A package read into GPU memory holds 0x48-byte texture records,
// each starting with a prebuilt T# whose base address is 0 until the loader (0x900688100) fills it
// in. The registry keeps every such template by dwords 1 and 2 (format, size); a T# a
// draw binds is a package texture when a template with the same bytes now holds its base address.
// Every 10 s a "Texture registry" line gives the share of bindings that are package textures.
namespace Libs::Graphics::TextureRegistry {

[[nodiscard]] bool Enabled();
// Any thread: `size` bytes of `path` were read into guest memory at `destination`.
void NotePackageRead(uint64_t destination, uint64_t size, const std::string& path);
// GPU thread: the T#s one draw or dispatch stage binds (`bindless`: which of them are bindless).
void NoteBindings(std::span<const ShaderRecompiler::IR::DescriptorValue> images,
                  std::span<const uint8_t> bindless);

} // namespace Libs::Graphics::TextureRegistry

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTUREREGISTRY_H_
