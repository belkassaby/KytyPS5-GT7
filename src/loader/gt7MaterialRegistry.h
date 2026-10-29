#ifndef KYTY_LOADER_GT7MATERIALREGISTRY_H_
#define KYTY_LOADER_GT7MATERIALREGISTRY_H_

#include <array>
#include <cstdint>
#include <utility>

// GT7 port, texture fast path (step 4; docs/GT7-RENDER-FRONTEND.md): GT7's material constant
// blocks, registered when gt7_ResolveMaterialTextures (eboot 0x900689e80) fills them at load. A
// material's block holds its textures' T#s and samplers' S#s at fixed offsets and is not written
// again until the next resolve, so the renderer can key work on a block's address. On with
// KYTY_TEXTURE_COUNTS=1 or KYTY_MATERIAL_REGISTRY=1. The hooked code is checked against GT7
// v01.710's first; on any other nothing is patched.
namespace Loader::Gt7MaterialRegistry {

// After the main program is loaded and relocated, before it runs.
void Install(uint64_t main_base);

[[nodiscard]] bool Enabled();

struct Block {
	uint64_t address = 0;
	uint32_t size    = 0;
	uint64_t set     = 0; // the material set it belongs to
	uint32_t index   = 0; // its material in the set
	uint64_t generation = 0; // the registry's, when registered
};

// The registered block containing [address, address + size), if any.
[[nodiscard]] bool Find(uint64_t address, uint32_t size, Block* block);

// Counts for reports: blocks and sets registered so far, and the registry's generation (bumped on
// every resolve, so cached answers can be dropped).
struct Totals {
	uint64_t sets       = 0;
	uint64_t blocks     = 0;
	uint64_t live       = 0; // blocks registered now
	uint64_t generation = 0;
};
[[nodiscard]] Totals GetTotals();

// The immutability check (diagnostics): compares every live block with its bytes when first
// checked after it was registered. CheckImmutabilityStep does a few blocks of a pass; it reads
// guest memory, so only on a guest thread (where Kyty handles faults on its protected pages; a
// host thread's read of one ended the game). CheckImmutability: the last finished pass.
void CheckImmutabilityStep(uint32_t blocks);
struct Immutability {
	uint64_t passes      = 0;
	uint64_t checked     = 0; // blocks compared (with a baseline taken earlier)
	uint64_t baselined   = 0; // blocks seen for the first time now
	uint64_t unmapped    = 0; // blocks no longer in committed memory
	uint64_t changed     = 0; // blocks that differ from their baseline now
	uint64_t changed_now = 0; // ... and differ from what the last check saw
	// the busiest changed byte offsets in a block (offset, blocks), at most 6
	std::array<std::pair<uint32_t, uint64_t>, 6> offsets {};
	uint32_t offset_count = 0;
};
[[nodiscard]] Immutability CheckImmutability();

} // namespace Loader::Gt7MaterialRegistry

#endif // KYTY_LOADER_GT7MATERIALREGISTRY_H_
