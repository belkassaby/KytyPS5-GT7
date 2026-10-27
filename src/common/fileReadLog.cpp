#include "common/fileReadLog.h"

#include <cstdlib>
#include <map>
#include <mutex>

namespace Common::FileReadLog {

namespace {

std::mutex                     g_mutex;
// By destination; a later read into the same place replaces the earlier one.
std::map<uint64_t, Read>       g_reads;
constexpr size_t               MaxReads = 1u << 20u;

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
