#include "common/fileReadLog.h"

#include "common/common.h"
#include "common/guestWatch.h"

#include <algorithm>
#include <cstdlib>
#include <fmt/format.h>
#include <map>
#include <mutex>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep
#endif

namespace Common::FileReadLog {

namespace {

std::mutex                     g_mutex;
// By destination; a later read into the same place replaces the earlier one.
std::map<uint64_t, Read>       g_reads;
constexpr size_t               MaxReads = 1u << 20u;

// KYTY_MAP_TSHARP_TEMPLATES=1: GT7's texture packages carry prebuilt T#s whose base address is 0
// in the file and filled in after the read. The first four such templates read into the GPU
// memory range (0x10 to 0x1f00000000) get a write watch on their first 8 bytes, so the code that
// fills in the address is logged (with KYTY_GUEST_WATCH_TSHARP=1, only that write).
void WatchTemplates(uint64_t destination, uint64_t size, const std::string& path) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	static const bool enabled = std::getenv("KYTY_MAP_TSHARP_TEMPLATES") != nullptr;
	static uint32_t   armed   = 0;
	if (!enabled || armed >= 4 || destination < 0x1000000000ull || destination >= 0x2000000000ull) {
		return;
	}
	constexpr uint64_t                Scanned = 16u << 20u;
	static std::vector<uint32_t> bytes;
	const auto                        length  = std::min(size, Scanned) & ~uint64_t {31};
	bytes.resize(length / 4);
	SIZE_T got = 0;
	if (length == 0 || ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(destination),
	                                     bytes.data(), length, &got) == FALSE) {
		return;
	}
	for (uint64_t i = 0; i + 4 <= got / 4; i += 8) { // 32-byte aligned
		// dword 0 (base address) 0; dword 1: GT7's format nibble 0xc, base high byte 0; dwords 2
		// and 3 (size, swizzle) set.
		if (bytes[i] == 0 && (bytes[i + 1] >> 28u) == 0xcu && (bytes[i + 1] & 0xffu) == 0 &&
		    bytes[i + 2] != 0 && bytes[i + 3] != 0) {
			const uint64_t address = destination + i * 4;
			const auto     reason  = fmt::format("T# template at +0x{:x} of a read of {}",
			                                     address - destination, path);
			GuestWatch::Arm(&address, 1, reason.c_str());
			armed++;
			return; // one a read
		}
	}
#else
	(void)destination;
	(void)size;
	(void)path;
#endif
}

} // namespace

bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_MAP_TEXTURES");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

void Record(uint64_t destination, uint64_t size, const std::string& path, uint64_t file_offset) {
	if (!Enabled() || size == 0) {
		return;
	}
	std::scoped_lock lock(g_mutex);
	// Reads this one covers are forgotten (their bytes were replaced).
	auto it = g_reads.lower_bound(destination);
	while (it != g_reads.end() && it->first < destination + size) {
		it = g_reads.erase(it);
	}
	if (g_reads.size() >= MaxReads) {
		g_reads.erase(g_reads.begin());
	}
	g_reads[destination] = {destination, size, file_offset, path};
	WatchTemplates(destination, size, path);
}

bool Find(uint64_t address, Read* read) {
	std::scoped_lock lock(g_mutex);
	auto             it = g_reads.upper_bound(address);
	if (it == g_reads.begin()) {
		return false;
	}
	--it;
	if (address >= it->second.destination + it->second.size) {
		return false;
	}
	if (read != nullptr) {
		*read = it->second;
	}
	return true;
}

} // namespace Common::FileReadLog
