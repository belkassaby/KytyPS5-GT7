#include "libs/agcCallSites.h"

#include "common/logging/log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::AgcCallSites {

namespace {

// Guest code (the eboot and the game's modules) lies here; the heap and the stacks do not.
constexpr uint64_t GuestCodeBegin = 0x800000000ull;
constexpr uint64_t GuestCodeEnd   = 0x1000000000ull;
constexpr size_t   SlotCount      = 1u << 16u;
constexpr auto     ReportEvery    = std::chrono::seconds(30);

struct Slot {
	std::atomic<uint64_t> key {0};
	const char*           function = nullptr;
	std::array<uint64_t, 3> chain {};
	std::atomic<uint64_t> count {0};
};

std::array<Slot, SlotCount> g_slots;
std::atomic<uint64_t>       g_dropped {0};
std::mutex                  g_report_mutex;
std::chrono::steady_clock::time_point g_last_report = std::chrono::steady_clock::now();
std::unordered_map<std::string, uint64_t> g_last_function_totals;

bool IsGuestCode(uint64_t address) {
	return address >= GuestCodeBegin && address < GuestCodeEnd;
}

uint64_t Mix(uint64_t value) {
	value ^= value >> 33u;
	value *= 0xff51afd7ed558ccdull;
	value ^= value >> 33u;
	return value;
}

bool IsDrawOrDispatch(const char* function) {
	return std::strstr(function, "Draw") != nullptr || std::strstr(function, "Dispatch") != nullptr;
}

void Report(double seconds) {
	struct Site {
		const char*             function;
		std::array<uint64_t, 3> chain;
		uint64_t                count;
	};
	std::vector<Site> sites;
	for (auto& slot: g_slots) {
		const auto count = slot.count.exchange(0, std::memory_order_relaxed);
		if (count != 0 && slot.function != nullptr) {
			sites.push_back({slot.function, slot.chain, count});
		}
	}
	std::unordered_map<std::string, uint64_t> by_function;
	std::unordered_map<uint64_t, uint64_t>    by_return;
	std::unordered_map<uint64_t, std::string> return_function;
	uint64_t                                  total = 0;
	for (const auto& site: sites) {
		by_function[site.function] += site.count;
		by_return[site.chain[0]] += site.count;
		return_function[site.chain[0]] = site.function;
		total += site.count;
	}
	std::vector<std::pair<std::string, uint64_t>> functions(by_function.begin(), by_function.end());
	std::ranges::sort(functions, [](const auto& a, const auto& b) { return a.second > b.second; });
	std::string line = fmt::format("Agc call sites ({:.0f} s): {:.0f} calls/s, {} sites, {} dropped |",
	                               seconds, static_cast<double>(total) / seconds, sites.size(),
	                               g_dropped.exchange(0));
	for (size_t i = 0; i < std::min<size_t>(functions.size(), 40); i++) {
		line += fmt::format(" {}={:.0f}/s", functions[i].first,
		                    static_cast<double>(functions[i].second) / seconds);
	}
	LOGF("%s\n", line.c_str());
	std::vector<std::pair<uint64_t, uint64_t>> returns(by_return.begin(), by_return.end());
	std::ranges::sort(returns, [](const auto& a, const auto& b) { return a.second > b.second; });
	LOGF("Agc call sites: %zu distinct return addresses; the busiest:\n", returns.size());
	for (size_t i = 0; i < std::min<size_t>(returns.size(), 60); i++) {
		LOGF("\t ret=0x%011" PRIx64 " %.0f/s %s\n", returns[i].first,
		     static_cast<double>(returns[i].second) / seconds, return_function[returns[i].first].c_str());
	}
	std::ranges::sort(sites, [](const Site& a, const Site& b) { return a.count > b.count; });
	LOGF("Agc call sites: the busiest draw and dispatch call sites:\n");
	size_t shown = 0;
	for (const auto& site: sites) {
		if (!IsDrawOrDispatch(site.function)) {
			continue;
		}
		LOGF("\t %.0f/s %s <- 0x%011" PRIx64 "\n", static_cast<double>(site.count) / seconds,
		     site.function, site.chain[0]);
		if (++shown == 40) {
			break;
		}
	}
}

} // namespace

bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_AGC_CALLSITES");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

void Record(const char* function, const void* return_address, const void* frame) {
	const auto ret = reinterpret_cast<uint64_t>(return_address);
	if (!IsGuestCode(ret)) {
		return; // a call from the emulator's own code (one Agc function calling another)
	}
	// Only the return address: the guest's stack above it can lie in pages the memory tracker
	// protects, where a read here would fault on every call (its callers are found in the code).
	(void)frame;
	const std::array<uint64_t, 3> chain {ret, 0, 0};
	const uint64_t key = Mix(reinterpret_cast<uint64_t>(function) ^ Mix(chain[0]) ^
	                         Mix(chain[1] * 3u) ^ Mix(chain[2] * 7u)) |
	                     1u;
	for (size_t probe = 0; probe < 16; probe++) {
		auto&    slot     = g_slots[(key + probe) & (SlotCount - 1u)];
		uint64_t expected = slot.key.load(std::memory_order_acquire);
		if (expected == 0 && slot.key.compare_exchange_strong(expected, key)) {
			slot.function = function;
			slot.chain    = chain;
		}
		if (expected == 0 || expected == key) {
			slot.count.fetch_add(1, std::memory_order_relaxed);
			break;
		}
		if (probe == 15) {
			g_dropped.fetch_add(1, std::memory_order_relaxed);
		}
	}
	thread_local uint32_t calls = 0;
	if ((++calls & 4095u) == 0) {
		const auto now = std::chrono::steady_clock::now();
		if (now - g_last_report >= ReportEvery && g_report_mutex.try_lock()) {
			const auto seconds = std::chrono::duration<double>(now - g_last_report).count();
			if (now - g_last_report >= ReportEvery) {
				g_last_report = now;
				Report(seconds);
			}
			g_report_mutex.unlock();
		}
	}
}

} // namespace Libs::Graphics::AgcCallSites
