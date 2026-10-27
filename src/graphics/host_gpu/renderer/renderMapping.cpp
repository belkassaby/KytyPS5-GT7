#include "graphics/host_gpu/renderer/renderMapping.h"

#include "common/file.h"
#include "common/fileReadLog.h"
#include "common/guestWatch.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/shader/recompiler/ir/ResourceSnapshot.h"
#include "kernel/memory.h"

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
constexpr size_t   MaxPlaces     = 64;            // places logged for each
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

// Whether the 32 bytes at `address` lie in memory the guest has mapped.
bool IsGuest(uint64_t address) {
	return LibKernel::Memory::TryClampRangeSize(address, 32) == 32;
}

bool ReadableProtection(DWORD protect) {
	return (protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
	       (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
	                   PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0;
}

struct Place {
	uint64_t address    = 0;
	bool     protected_ = false; // found through the backing store: the page has no CPU access
};

// Every 8-byte-aligned place in guest memory whose 16 bytes are one of `keys`. Readable pages are
// copied with ReadProcessMemory (a protection the tracker changes meanwhile makes the copy fail
// instead of faulting this thread); pages without CPU access (GPU-written, or read-watched) are
// read from the guest's backing store.
std::vector<std::vector<Place>> Search(const std::vector<Key>& keys, uint64_t& scanned,
                                       uint64_t& scanned_protected) {
	std::vector<std::vector<Place>> places(keys.size());
	scanned           = 0;
	scanned_protected = 0;
	static std::vector<uint64_t> chunk(ChunkBytes / 8 + 1);
	const auto match = [&](uint64_t at, uint64_t got, bool protected_) {
		const auto count = got / 8 - 1;
		for (uint64_t i = 0; i < count && i < ChunkBytes / 8; i++) {
			const auto first = chunk[i];
			for (size_t k = 0; k < keys.size(); k++) {
				uint64_t want = 0;
				std::memcpy(&want, keys[k].dwords.data(), 8);
				// Guest memory only: the emulator keeps its own copies of the T#s it binds in its
				// heap, which lies in the same address range.
				if (first == want && std::memcmp(&chunk[i + 1], &keys[k].dwords[2], 8) == 0 &&
				    places[k].size() < MaxPlaces && IsGuest(at + i * 8)) {
					places[k].push_back({at + i * 8, protected_});
				}
			}
		}
	};
	for (uint64_t address = GuestBegin; address < GuestEnd;) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0) {
			break;
		}
		const auto base = reinterpret_cast<uint64_t>(info.BaseAddress);
		const auto end  = base + info.RegionSize;
		if (info.State == MEM_COMMIT && info.RegionSize >= 16) {
			const bool readable = ReadableProtection(info.Protect);
			for (uint64_t at = base; at < end; at += ChunkBytes) {
				const auto bytes = std::min<uint64_t>(ChunkBytes + 8, end - at);
				uint64_t   got   = 0;
				if (readable) {
					SIZE_T copied = 0;
					if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at),
					                      chunk.data(), bytes, &copied) != FALSE) {
						got = copied;
					}
					scanned += got;
				} else {
					got = LibKernel::Memory::TryClampRangeSize(at, bytes);
					if (got != 0 && !LibKernel::Memory::TryReadBacking(at, chunk.data(), got)) {
						got = 0;
					}
					scanned_protected += got;
				}
				if (got >= 16) {
					match(at, got, !readable);
				}
			}
		}
		address = std::max(end, address + 0x1000);
	}
	return places;
}

// Where the bytes at `address` came from (Common::FileReadLog): the file and offset of the read
// that filled them, and whether that file holds the same T# there (prebuilt) or other bytes (the
// guest changed them after the read: base address or flags patched).
std::string FromFile(uint64_t address, const std::array<uint32_t, 4>& tsharp) {
	Common::FileReadLog::Read read;
	if (!Common::FileReadLog::Find(address, &read)) {
		return ", not from a file read";
	}
	const auto   offset = read.file_offset + (address - read.destination);
	Common::File file;
	std::array<uint32_t, 4> in_file {};
	uint32_t                got = 0;
	if (file.Open(read.path, Common::File::Mode::Read) && file.Seek(offset)) {
		file.Read(in_file.data(), sizeof(in_file), &got);
	}
	file.Close();
	const auto bytes = got == sizeof(in_file)
	                       ? fmt::format("{:08x} {:08x} {:08x} {:08x}", in_file[0], in_file[1],
	                                     in_file[2], in_file[3])
	                       : std::string("unreadable");
	return fmt::format(", read from {} at 0x{:x} (a read of 0x{:x} bytes into 0x{:x}); the file "
	                   "has {} there: {}",
	                   read.path, offset, read.size, read.destination, bytes,
	                   in_file == tsharp ? "the same T#" : "other bytes");
}

void Map(State& state, BufferCache& buffers) {
	std::vector<std::pair<Key, uint64_t>> busiest;
	// Mipmapped textures only (KYTY_MAP_TEXTURES_ALL=1: any): the busiest single-level ones are
	// render targets, whose T#s live only in the per-frame tables. A T#'s last mip level is
	// dword 3, bits 16 to 19.
	static const bool all = std::getenv("KYTY_MAP_TEXTURES_ALL") != nullptr;
	for (const auto& [key, uses]: state.uses) {
		if (all || ((key.dwords[3] >> 16u) & 0xfu) != 0) {
			busiest.emplace_back(key, uses);
		}
	}
	std::ranges::sort(busiest, [](const auto& a, const auto& b) { return a.second > b.second; });
	busiest.resize(std::min(busiest.size(), Searched));
	std::vector<Key> keys;
	for (const auto& [key, uses]: busiest) {
		keys.push_back(key);
	}
	const auto started           = std::chrono::steady_clock::now();
	uint64_t   scanned           = 0;
	uint64_t   scanned_protected = 0;
	const auto places            = Search(keys, scanned, scanned_protected);
	LOGF("Texture map: %zu distinct T#s bound; searched %.1f GB of readable and %.1f GB of protected "
	     "guest memory for the %zu busiest in %.1f s\n",
	     state.uses.size(), static_cast<double>(scanned) / (1u << 30u),
	     static_cast<double>(scanned_protected) / (1u << 30u), keys.size(),
	     std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
	uint32_t armed = 0;
	for (size_t k = 0; k < keys.size(); k++) {
		const auto& d    = keys[k].dwords;
		const auto  base = (static_cast<uint64_t>(d[0]) | (static_cast<uint64_t>(d[1] & 0xffu) << 32u))
		                  << 8u;
		LOGF("Texture map: T# %08x %08x %08x %08x (texture at 0x%" PRIx64 ") bound %" PRIu64
		     " times, at %zu places%s\n",
		     d[0], d[1], d[2], d[3], base, busiest[k].second, places[k].size(),
		     places[k].empty() ? " (nowhere)" : "");
		// Each place: whether the CPU can read it, whether the GPU wrote those bytes (the buffer
		// cache's record), and which shaders wrote them last (with KYTY_GPU_WRITERS=1).
		for (const auto& place: places[k]) {
			const bool gpu_dirty = buffers.HasGpuDirtyBytes(place.address, 32);
			const auto writers   = BufferCache::GpuWritersOf(place.address, 32);
			LOGF("Texture map:   0x%" PRIx64 " %s, %s, last GPU writers: %s%s\n", place.address,
			     place.protected_ ? "protected" : "readable",
			     gpu_dirty ? "GPU-written (not downloaded)" : "CPU copy current",
			     writers.empty() ? "-" : writers.c_str(), FromFile(place.address, d).c_str());
		}
		// One watch for each of the four busiest T#s found in readable memory: the place with the
		// highest address (a texture package header in GPU memory lies above the per-frame tables).
		for (auto place = places[k].rbegin(); place != places[k].rend(); ++place) {
			if (armed < 4 && !place->protected_) {
				const auto reason = fmt::format("T# of texture 0x{:x}", base);
				if (Common::GuestWatch::ArmExpecting(place->address, reason.c_str()) != 0) {
					armed++;
				}
				break;
			}
		}
	}
}

#else

void Map(State& /*state*/, BufferCache& /*buffers*/) {
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
		// A watch on the emulator's own memory fires inside its allocator: guest memory only.
		// The GPU memory range (0x1000000000 to 0x1fffffffff) holds no emulator memory, so an
		// address there is taken even while unmapped (a texture package the next race loads).
		if (LibKernel::Memory::TryClampRangeSize(value, 8) == 8 ||
		    (value >= 0x1000000000ull && value < 0x2000000000ull)) {
			addresses.push_back(value);
		} else {
			LOGF("Texture map: 0x%llx is not guest memory; not watched\n", value);
		}
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
                  std::span<const uint8_t> bindless, uint64_t frame, BufferCache& buffers) {
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
			Map(state, buffers);
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
