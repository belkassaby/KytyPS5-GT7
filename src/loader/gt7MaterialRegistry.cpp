#include "loader/gt7MaterialRegistry.h"

#include "common/abi.h"
#include "common/logging/log.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "loader/gt7GuestHook.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include <fmt/format.h>

namespace Loader::Gt7MaterialRegistry {

namespace {

constexpr uint64_t Resolve = 0x689e80; // gt7_ResolveMaterialTextures(set, package)

// push rbp; mov rbp, rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp, 0x38
constexpr std::array<uint8_t, 17> ResolveMoved = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57,
                                                  0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                                  0x53, 0x48, 0x83, 0xec, 0x38};

// The material set (docs/GT7-RENDER-FRONTEND.md): count at +0xc, GT7Material (0x30 bytes) at
// +0x48, GT7MaterialShader (0x28 bytes) at +0x50. A material's block is at +0x18, its shader
// index at +0x8; the shader's block size at +0x8.
constexpr uint64_t SetCount     = 0xc;
constexpr uint64_t SetMaterials = 0x48;
constexpr uint64_t SetShaders   = 0x50;
constexpr uint64_t MaterialSize = 0x30;
constexpr uint64_t ShaderSize   = 0x28;
constexpr uint32_t MaxMaterials = 1u << 16u;

bool g_enabled = false;

std::shared_mutex          g_mutex;
std::map<uint64_t, Block>  g_blocks; // by address
std::atomic<uint64_t>      g_sets {0};
std::atomic<uint64_t>      g_registered {0};
std::atomic<uint64_t>      g_generation {0};

template <typename T>
T Read(uint64_t address) {
	T value {};
	std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
	return value;
}

KYTY_SYSV_ABI void OnResolve(uint64_t set, uint64_t /*package*/) {
	const auto count     = Read<uint32_t>(set + SetCount);
	const auto materials = Read<uint64_t>(set + SetMaterials);
	const auto shaders   = Read<uint64_t>(set + SetShaders);
	if (count == 0 || count > MaxMaterials || materials == 0 || shaders == 0) {
		return;
	}
	const auto generation = g_generation.fetch_add(1, std::memory_order_relaxed) + 1;
	uint64_t   added      = 0;
	std::unique_lock lock(g_mutex);
	for (uint32_t i = 0; i < count; i++) {
		const auto material = materials + i * MaterialSize;
		const auto address  = Read<uint64_t>(material + 0x18);
		if (address == 0) {
			continue;
		}
		const auto shader = shaders + Read<uint16_t>(material + 0x8) * ShaderSize;
		const auto size   = Read<uint16_t>(shader + 0x8);
		if (size == 0) {
			continue;
		}
		// a block registered where another was (memory reused after an unload) replaces it
		g_blocks.erase(g_blocks.lower_bound(address), g_blocks.lower_bound(address + size));
		if (auto before = g_blocks.lower_bound(address); before != g_blocks.begin()) {
			--before;
			if (before->second.address + before->second.size > address) {
				g_blocks.erase(before);
			}
		}
		g_blocks[address] = Block {address, size, set, i, generation};
		added++;
	}
	g_sets.fetch_add(1, std::memory_order_relaxed);
	g_registered.fetch_add(added, std::memory_order_relaxed);
}

} // namespace

bool Enabled() {
	return g_enabled;
}

bool Find(uint64_t address, uint32_t size, Block* block) {
	std::shared_lock lock(g_mutex);
	auto it = g_blocks.upper_bound(address);
	if (it == g_blocks.begin()) {
		return false;
	}
	--it;
	if (address + size > it->second.address + it->second.size) {
		return false;
	}
	*block = it->second;
	return true;
}

namespace {

struct Baseline {
	uint64_t             generation = 0; // the block's registration it was taken for
	std::vector<uint8_t> bytes;
	std::vector<uint8_t> last;
};
// A pass over a snapshot of the live blocks, a few blocks a step; the last finished pass is what
// CheckImmutability reports.
struct Pass {
	std::vector<Block>           blocks;
	size_t                       cursor = 0;
	Immutability                 result;
	std::map<uint32_t, uint64_t> offsets;
};
std::mutex                   g_check_mutex; // the pass, the baselines, the last result
std::map<uint64_t, Baseline> g_baselines;   // by block address
Pass                         g_pass;
Immutability                 g_last;
std::vector<uint8_t>         g_now;

void FinishPass() {
	std::vector<std::pair<uint32_t, uint64_t>> sorted(g_pass.offsets.begin(), g_pass.offsets.end());
	std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second > b.second; });
	for (size_t i = 0; i < std::min(sorted.size(), g_pass.result.offsets.size()); i++) {
		g_pass.result.offsets[i] = sorted[i];
		g_pass.result.offset_count++;
	}
	g_pass.result.passes = g_last.passes + 1;
	g_last               = g_pass.result;
	g_pass.result        = {};
	g_pass.offsets.clear();
	g_pass.cursor = 0;
	g_pass.blocks.clear();
	std::shared_lock lock(g_mutex);
	g_pass.blocks.reserve(g_blocks.size());
	for (const auto& [address, block]: g_blocks) {
		g_pass.blocks.push_back(block);
	}
}

void CheckBlock(const Block& block) {
	auto& result = g_pass.result;
	if (!Libs::LibKernel::Memory::IsCommittedRange(block.address, block.size)) {
		result.unmapped++;
		return;
	}
	g_now.resize(block.size);
	std::memcpy(g_now.data(), reinterpret_cast<const void*>(block.address), block.size);
	auto& baseline = g_baselines[block.address];
	if (baseline.generation != block.generation || baseline.bytes.size() != block.size) {
		baseline.generation = block.generation;
		baseline.bytes      = g_now;
		baseline.last       = g_now;
		result.baselined++;
		return;
	}
	result.checked++;
	if (g_now != baseline.bytes) {
		result.changed++;
		for (uint32_t i = 0; i < block.size; i += 4) {
			if (std::memcmp(g_now.data() + i, baseline.bytes.data() + i,
			                std::min<uint32_t>(4, block.size - i)) != 0) {
				g_pass.offsets[i]++;
			}
		}
	}
	if (g_now != baseline.last) {
		result.changed_now++;
		baseline.last = g_now;
	}
}

} // namespace

void CheckImmutabilityStep(uint32_t blocks) {
	std::unique_lock lock(g_check_mutex, std::try_to_lock);
	if (!lock.owns_lock()) {
		return;
	}
	for (uint32_t i = 0; i < blocks; i++) {
		if (g_pass.cursor >= g_pass.blocks.size()) {
			FinishPass();
			return;
		}
		CheckBlock(g_pass.blocks[g_pass.cursor++]);
	}
}

Immutability CheckImmutability() {
	std::scoped_lock lock(g_check_mutex);
	return g_last;
}

Totals GetTotals() {
	Totals totals;
	totals.sets       = g_sets.load(std::memory_order_relaxed);
	totals.blocks     = g_registered.load(std::memory_order_relaxed);
	totals.generation = g_generation.load(std::memory_order_relaxed);
	std::shared_lock lock(g_mutex);
	totals.live = g_blocks.size();
	return totals;
}

void Install(uint64_t main_base) {
	const char* counts   = std::getenv("KYTY_TEXTURE_COUNTS");
	const char* registry = std::getenv("KYTY_MATERIAL_REGISTRY");
	if (!((counts != nullptr && counts[0] == '1') || (registry != nullptr && registry[0] == '1'))) {
		return;
	}
	auto* resolve = reinterpret_cast<uint8_t*>(main_base + Resolve);
	if (std::memcmp(resolve, ResolveMoved.data(), ResolveMoved.size()) != 0) {
		Log::WriteToConsoleAndLog("Material registry: not installed (gt7_ResolveMaterialTextures "
		                          "differs from GT7 v01.710's)\n");
		return;
	}
	const auto page = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    main_base, 0x4000, Common::VirtualMemory::Mode::ExecuteReadWrite, "gt7_material_registry",
	    false);
	if (page == 0) {
		Log::WriteToConsoleAndLog("Material registry: not installed (no memory for the stub)\n");
		return;
	}
	Gt7GuestHook::Emitter e {reinterpret_cast<uint8_t*>(page)};
	const auto stub = e.Here();
	e.CallHost(reinterpret_cast<const void*>(&OnResolve)); // rdi: the set, rsi: the package
	e.Return(ResolveMoved, main_base + Resolve);
	Common::VirtualMemory::FlushInstructionCache(page, 0x200);
	Gt7GuestHook::PatchEntry(resolve, ResolveMoved.size(), stub);
	g_enabled = true;
	Log::WriteToConsoleAndLog("Material registry: installed on gt7_ResolveMaterialTextures\n");
}

} // namespace Loader::Gt7MaterialRegistry
