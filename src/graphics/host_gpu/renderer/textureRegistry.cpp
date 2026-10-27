#include "graphics/host_gpu/renderer/textureRegistry.h"

#include "common/logging/log.h"
#include "graphics/shader/recompiler/ir/ResourceSnapshot.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep
#endif

namespace Libs::Graphics::TextureRegistry {

namespace {

constexpr uint64_t GpuBegin = 0x1000000000ull;
constexpr uint64_t GpuEnd   = 0x2000000000ull;

// A template's identity: T# dwords 1 and 2 with the base address's high byte (dword 1, bits 0 to
// 7) cleared, as the file has them (the other five are 0 here).
struct Signature {
	std::array<uint32_t, 7> dwords {};
	bool operator==(const Signature&) const = default;
};

struct SignatureHash {
	size_t operator()(const Signature& s) const {
		uint64_t h = 0xcbf29ce484222325ull;
		for (const auto d: s.dwords) {
			h = (h ^ d) * 0x100000001b3ull;
		}
		return static_cast<size_t>(h);
	}
};

struct Registry {
	std::mutex                                                               mutex;
	std::unordered_map<Signature, std::vector<uint64_t>, SignatureHash>     templates;
	uint64_t                                                                 template_count = 0;
	uint64_t                                                                 packages       = 0;
	// GPU thread only.
	uint64_t                     bindings        = 0;
	uint64_t                     matched         = 0;
	uint64_t                     unmatched_mips  = 0; // mipmapped T#s no template explains
	std::unordered_set<uint64_t> matched_bases;
	std::unordered_set<uint64_t> unmatched_bases;
	std::chrono::steady_clock::time_point last_report = std::chrono::steady_clock::now();
};

Registry& Get() {
	static Registry registry;
	return registry;
}

bool LooksLikeTemplate(const uint32_t* d) {
	return d[0] == 0 && (d[1] >> 28u) == 0xcu && (d[1] & 0xffu) == 0 && d[2] != 0 && d[3] != 0;
}

// Dwords 1 and 2 only (format, size): a draw's T# can differ from the file's in the others (dword
// 3's top bits, the LOD fields); the base address the template then holds settles which it is.
Signature SignatureOf(const uint32_t* d) {
	Signature s;
	s.dwords[0] = d[1] & ~0xffu;
	s.dwords[1] = d[2];
	return s;
}

bool ReadGuest(uint64_t address, void* data, size_t size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	SIZE_T got = 0;
	return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), data, size,
	                         &got) != FALSE &&
	       got == size;
#else
	std::memcpy(data, reinterpret_cast<const void*>(address), size);
	return true;
#endif
}

void Report(Registry& r) {
	const auto now = std::chrono::steady_clock::now();
	if (now - r.last_report < std::chrono::seconds(10)) {
		return;
	}
	r.last_report = now;
	uint64_t templates = 0, packages = 0;
	{
		std::scoped_lock lock(r.mutex);
		templates = r.template_count;
		packages  = r.packages;
	}
	LOGF("Texture registry (10 s): %" PRIu64 " packages, %" PRIu64 " T# templates; %" PRIu64
	     " bindings, %.1f%% package textures (%zu distinct), %" PRIu64
	     " mipmapped bindings no template explains (%zu distinct)\n",
	     packages, templates, r.bindings,
	     r.bindings != 0 ? 100.0 * static_cast<double>(r.matched) / static_cast<double>(r.bindings)
	                     : 0.0,
	     r.matched_bases.size(), r.unmatched_mips, r.unmatched_bases.size());
	r.bindings       = 0;
	r.matched        = 0;
	r.unmatched_mips = 0;
	r.matched_bases.clear();
	r.unmatched_bases.clear();
}

} // namespace

bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_TEXTURE_REGISTRY");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

void NotePackageRead(uint64_t destination, uint64_t size, const std::string& /*path*/) {
	if (!Enabled() || destination < GpuBegin || destination >= GpuEnd || size < 32) {
		return;
	}
	// The texture records lie in the package's header: its first 16 MB are enough.
	const auto length = std::min<uint64_t>(size, 16u << 20u) & ~uint64_t {31};
	std::vector<uint32_t> bytes(length / 4);
	if (!ReadGuest(destination, bytes.data(), length)) {
		return;
	}
	std::vector<std::pair<Signature, uint64_t>> found;
	for (size_t i = 0; i + 8 <= bytes.size(); i += 8) { // 32-byte aligned
		if (LooksLikeTemplate(&bytes[i])) {
			found.emplace_back(SignatureOf(&bytes[i]), destination + i * 4);
		}
	}
	if (found.empty()) {
		return;
	}
	auto&            r = Get();
	std::scoped_lock lock(r.mutex);
	r.packages++;
	for (const auto& [signature, address]: found) {
		r.templates[signature].push_back(address);
		r.template_count++;
	}
}

void NoteBindings(std::span<const ShaderRecompiler::IR::DescriptorValue> images,
                  std::span<const uint8_t> bindless) {
	auto& r = Get();
	for (size_t i = 0; i < images.size(); i++) {
		if ((i < bindless.size() && bindless[i] != 0) || images[i].dword_count < 8) {
			continue;
		}
		const auto* d = images[i].dwords.data();
		if (d[0] == 0 && (d[1] & 0xffu) == 0) {
			continue; // a null T#
		}
		r.bindings++;
		const auto base = (static_cast<uint64_t>(d[0]) | (static_cast<uint64_t>(d[1] & 0xffu) << 32u))
		                  << 8u;
		bool matched = false;
		{
			std::scoped_lock lock(r.mutex);
			if (const auto it = r.templates.find(SignatureOf(d)); it != r.templates.end()) {
				for (const auto address: it->second) {
					uint32_t now[2] = {};
					if (ReadGuest(address, now, sizeof(now)) && now[0] == d[0] &&
					    (now[1] & 0xffu) == (d[1] & 0xffu)) {
						matched = true;
						break;
					}
				}
			}
		}
		if (matched) {
			r.matched++;
			r.matched_bases.insert(base);
		} else if (((d[3] >> 16u) & 0xfu) != 0) {
			r.unmatched_mips++;
			r.unmatched_bases.insert(base);
		}
	}
	Report(r);
}

} // namespace Libs::Graphics::TextureRegistry
