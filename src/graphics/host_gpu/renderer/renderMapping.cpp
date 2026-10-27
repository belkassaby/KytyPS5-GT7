#include "graphics/host_gpu/renderer/renderMapping.h"

#include "common/guestWatch.h"
#include "common/logging/log.h"
#include "graphics/shader/recompiler/ir/ResourceSnapshot.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <string>
#include <unordered_map>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep
#endif

namespace Libs::Graphics::RenderMapping {

namespace {

constexpr uint64_t HeavyBindings = 2000;          // a frame binding at least this many T#s
constexpr uint64_t HeavyFrames   = 300;           // in a row before the search (~15 s in a race)
constexpr size_t   Searched      = 8;             // the busiest T#s searched for
constexpr size_t   MaxPlaces     = 16;            // places logged for each
constexpr uint64_t GuestBegin    = 0x400000000ull;
constexpr uint64_t GuestEnd      = 0x10000000000ull;
constexpr uint64_t ChunkBytes    = 1u << 20u;

struct Key {
	std::array<uint32_t, 4> dwords {}; // the first 16 bytes: base, format, size, swizzle
	bool operator==(const Key&) const = default;
};

struct KeyHash {
	size_t operator()(const Key& key) const {
		uint64_t h = 0xcbf29ce484222325ull;
		for (const auto d: key.dwords) {
			h = (h ^ d) * 0x100000001b3ull;
		}
		return static_cast<size_t>(h);
	}
};

struct State {
	std::unordered_map<Key, uint64_t, KeyHash> uses;
	uint64_t                                   frame          = UINT64_MAX;
	uint64_t                                   frame_bindings = 0;
	uint64_t                                   heavy_frames   = 0;
	bool                                       done           = false;
};

State& GetState() {
	static State state;
	return state;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

bool ReadableProtection(DWORD protect) {
	return (protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
	       (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
	                   PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Every 8-byte-aligned place in readable guest memory whose 16 bytes are one of `keys`.
std::vector<std::vector<uint64_t>> Search(const std::vector<Key>& keys, uint64_t& scanned) {
	std::vector<std::vector<uint64_t>> places(keys.size());
	scanned = 0;
	for (uint64_t address = GuestBegin; address < GuestEnd;) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0) {
			break;
		}
		const auto base = reinterpret_cast<uint64_t>(info.BaseAddress);
		const auto end  = base + info.RegionSize;
		if (info.State == MEM_COMMIT && ReadableProtection(info.Protect) && info.RegionSize >= 16) {
			// Copied, not read in place: a protection the tracker changes meanwhile makes the copy
			// fail instead of faulting on this thread.
			static std::vector<uint64_t> chunk(ChunkBytes / 8 + 1);
			for (uint64_t at = base; at < end; at += ChunkBytes) {
				const auto bytes = std::min<uint64_t>(ChunkBytes + 8, end - at);
				SIZE_T     got   = 0;
				if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at),
				                      chunk.data(), bytes, &got) == FALSE ||
				    got < 16) {
					continue;
				}
				scanned += got;
				const auto count = got / 8 - 1;
				for (uint64_t i = 0; i < count && i < ChunkBytes / 8; i++) {
					const auto first = chunk[i];
					for (size_t k = 0; k < keys.size(); k++) {
						uint64_t want = 0;
						std::memcpy(&want, keys[k].dwords.data(), 8);
						if (first == want && std::memcmp(&chunk[i + 1], &keys[k].dwords[2], 8) == 0 &&
						    places[k].size() < MaxPlaces) {
							places[k].push_back(at + i * 8);
						}
					}
				}
			}
		}
		address = std::max(end, address + 0x1000);
	}
	return places;
}

void Map(State& state) {
	std::vector<std::pair<Key, uint64_t>> busiest(state.uses.begin(), state.uses.end());
	std::ranges::sort(busiest, [](const auto& a, const auto& b) { return a.second > b.second; });
	busiest.resize(std::min(busiest.size(), Searched));
	std::vector<Key> keys;
	for (const auto& [key, uses]: busiest) {
		keys.push_back(key);
	}
	const auto started = std::chrono::steady_clock::now();
	uint64_t   scanned = 0;
	const auto places  = Search(keys, scanned);
	LOGF("Texture map: %zu distinct T#s bound; searched %.1f GB of guest memory for the %zu busiest "
	     "in %.1f s\n",
	     state.uses.size(), static_cast<double>(scanned) / (1u << 30u), keys.size(),
	     std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
	uint32_t armed = 0;
	for (size_t k = 0; k < keys.size(); k++) {
		const auto& d    = keys[k].dwords;
		const auto  base = (static_cast<uint64_t>(d[0]) | (static_cast<uint64_t>(d[1] & 0xffu) << 32u))
		                  << 8u;
		std::string list;
		for (const auto place: places[k]) {
			list += fmt::format(" 0x{:x}", place);
		}
		LOGF("Texture map: T# %08x %08x %08x %08x (texture at 0x%" PRIx64 ") bound %" PRIu64
		     " times, at%s\n",
		     d[0], d[1], d[2], d[3], base, busiest[k].second, list.empty() ? " nowhere" : list.c_str());
		// One watch for each of the four busiest T#s found in memory: the place found first (the
		// lowest address).
		if (armed < 4 && !places[k].empty()) {
			const auto reason = fmt::format("T# of texture 0x{:x}", base);
			if (Common::GuestWatch::ArmExpecting(places[k].front(), reason.c_str()) != 0) {
				armed++;
			}
		}
	}
}

#else

void Map(State& /*state*/) {
	LOGF("Texture map: needs Windows\n");
}

#endif

// _mapping/arm.txt (in the working directory): hex addresses, up to four, that replace every
// watch with plain ones (Common::GuestWatch::Replace); the file is deleted once read. Lets the
// watches be moved while the game runs (before a race loads, onto the T# homes found in the last).
void CheckArmFile() {
	constexpr const char* Path = "_mapping/arm.txt";
	FILE*                 file = std::fopen(Path, "r");
	if (file == nullptr) {
		return;
	}
	std::vector<uint64_t> addresses;
	unsigned long long    value = 0;
	while (addresses.size() < 4 && std::fscanf(file, "%llx", &value) == 1) {
		addresses.push_back(value);
	}
	std::fclose(file);
	std::remove(Path);
	LOGF("Texture map: %s asks for %zu watches\n", Path, addresses.size());
	Common::GuestWatch::Replace(addresses.data(), static_cast<uint32_t>(addresses.size()),
	                            "_mapping/arm.txt");
}

} // namespace

bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_MAP_TEXTURES");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

void NoteTextures(std::span<const ShaderRecompiler::IR::DescriptorValue> images,
                  std::span<const uint8_t> bindless, uint64_t frame) {
	auto& state = GetState();
	if (frame != state.frame && frame % 120 == 0) {
		CheckArmFile();
	}
	if (state.done) {
		return;
	}
	if (frame != state.frame) {
		state.heavy_frames = state.frame_bindings >= HeavyBindings ? state.heavy_frames + 1 : 0;
		state.frame          = frame;
		state.frame_bindings = 0;
		if (state.heavy_frames >= HeavyFrames) {
			state.done = true;
			Map(state);
			return;
		}
	}
	for (size_t i = 0; i < images.size(); i++) {
		if ((i < bindless.size() && bindless[i] != 0) || images[i].dword_count < 4) {
			continue;
		}
		Key key;
		std::memcpy(key.dwords.data(), images[i].dwords.data(), sizeof(key.dwords));
		if (key.dwords[0] == 0 && (key.dwords[1] & 0xffu) == 0) {
			continue; // a null T#
		}
		state.uses[key]++;
		state.frame_bindings++;
	}
}

} // namespace Libs::Graphics::RenderMapping
