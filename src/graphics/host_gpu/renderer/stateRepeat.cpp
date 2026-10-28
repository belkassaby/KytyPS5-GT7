#include "graphics/host_gpu/renderer/stateRepeat.h"

#include "common/logging/log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace Libs::Graphics::StateRepeat {

namespace {

// Draws by how their registers compare with the previous draw's.
enum Kind : uint32_t {
	AllSame,     // context, shader and user config registers all unchanged
	ContextSame, // context registers unchanged, shader or user config registers not
	Changed,     // context registers changed
	KindCount
};

constexpr auto StepCount = static_cast<size_t>(Step::Count);

struct KindStats {
	uint64_t                         draws = 0;
	std::array<uint64_t, StepCount> count {};
	std::array<uint64_t, StepCount> same {};
	std::array<uint64_t, StepCount> cycles {};
};

// A copy of one register file, compared byte for byte (a value written changes its bytes, so a
// difference is never missed; padding written differently only shows as a change).
struct Snapshot {
	const void*          source = nullptr;
	std::vector<uint8_t> bytes;

	// Whether `data` holds what it held at the last call; keeps a copy of it either way.
	bool Same(const void* data, size_t size) {
		if (source == data && bytes.size() == size && std::memcmp(bytes.data(), data, size) == 0) {
			return true;
		}
		source = data;
		bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
		return false;
	}
};

constexpr size_t BlocksKept    = 16;
constexpr size_t DistinctKept  = 65536;
constexpr auto   ReportPeriod  = std::chrono::seconds(10);

// GPU thread only.
struct State {
	Snapshot context;
	Snapshot shaders;
	Snapshot user_config;
	Kind     kind = Changed;
	bool     open = false;

	std::array<uint64_t, StepCount> last_signature {};

	// The indirect context blocks applied since the last draw, and those of the draw before.
	std::array<uint64_t, BlocksKept> blocks {};
	uint32_t                         block_count = 0;
	std::array<uint64_t, BlocksKept> last_blocks {};
	uint32_t                         last_block_count = 0;

	std::array<KindStats, KindCount> stats {};
	uint64_t blocks_total       = 0; // blocks applied
	uint64_t blocks_repeated    = 0; // draws whose block list was the previous draw's
	uint64_t blocks_but_changed = 0; // of those, draws whose context registers changed anyway
	uint64_t same_other_blocks  = 0; // draws with unchanged context registers and other blocks
	std::unordered_set<uint64_t> distinct;

	std::chrono::steady_clock::time_point last_time = std::chrono::steady_clock::now();
	uint64_t                              last_tsc  = __rdtsc();
	uint64_t                              draws     = 0;
};

State& GetState() {
	static State state;
	return state;
}

std::string Describe(const KindStats& stats, uint64_t draws, double cycles_per_us) {
	static constexpr std::array<const char*, StepCount> names {"programs", "targets", "pipeline",
	                                                           "rendering", "image set"};
	auto text = fmt::format("{:.1f}%", draws != 0 ? 100.0 * static_cast<double>(stats.draws) /
	                                                    static_cast<double>(draws)
	                                              : 0.0);
	if (stats.draws == 0) {
		return text;
	}
	text += " (";
	for (size_t i = 0; i < StepCount; i++) {
		const auto n = stats.count[i];
		text += fmt::format(
		    "{}{} {:.1f}% as before, {:.2f} us", i != 0 ? "; " : "", names[i],
		    n != 0 ? 100.0 * static_cast<double>(stats.same[i]) / static_cast<double>(n) : 0.0,
		    n != 0 ? static_cast<double>(stats.cycles[i]) / cycles_per_us / static_cast<double>(n)
		           : 0.0);
	}
	return text + ")";
}

void ReportIfDue(State& state) {
	if ((state.draws & 1023u) != 0) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now - state.last_time < ReportPeriod) {
		return;
	}
	const auto   tsc     = __rdtsc();
	const double seconds = std::chrono::duration<double>(now - state.last_time).count();
	const double cycles_per_us =
	    static_cast<double>(tsc - state.last_tsc) / (seconds * 1'000'000.0);
	uint64_t draws = 0;
	for (const auto& stats: state.stats) {
		draws += stats.draws;
	}
	if (draws != 0 && cycles_per_us > 0.0) {
		const auto share = [&](uint64_t n) {
			return 100.0 * static_cast<double>(n) / static_cast<double>(draws);
		};
		LOGF("State repeats (%.1f s): %" PRIu64 " draws; registers all as before %s; context as "
		     "before %s; context changed %s | context blocks: %.2f a draw, the same as the draw "
		     "before %.1f%% (context changed anyway %.1f%%), context as before with other blocks "
		     "%.1f%%, %zu%s distinct\n",
		     seconds, draws, Describe(state.stats[AllSame], draws, cycles_per_us).c_str(),
		     Describe(state.stats[ContextSame], draws, cycles_per_us).c_str(),
		     Describe(state.stats[Changed], draws, cycles_per_us).c_str(),
		     static_cast<double>(state.blocks_total) / static_cast<double>(draws),
		     share(state.blocks_repeated), share(state.blocks_but_changed),
		     share(state.same_other_blocks), state.distinct.size(),
		     state.distinct.size() >= DistinctKept ? " or more" : "");
	}
	state.stats              = {};
	state.blocks_total       = 0;
	state.blocks_repeated    = 0;
	state.blocks_but_changed = 0;
	state.same_other_blocks  = 0;
	state.distinct.clear();
	state.last_time = now;
	state.last_tsc  = tsc;
}

} // namespace

bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_STATE_REPEAT");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

void NoteCxBlock(uint64_t address, uint32_t count) {
	if (!Enabled()) {
		return;
	}
	auto& state = GetState();
	if (state.block_count < BlocksKept) {
		state.blocks[state.block_count] = address ^ (static_cast<uint64_t>(count) << 48u);
	}
	state.block_count++;
	if (state.distinct.size() < DistinctKept) {
		state.distinct.insert(address);
	}
}

void BeginDraw(const void* context, size_t context_size, const void* shaders, size_t shaders_size,
               const void* user_config, size_t user_config_size) {
	if (!Enabled()) {
		return;
	}
	auto&      state        = GetState();
	const bool context_same = state.context.Same(context, context_size);
	// Both compared every draw, so each snapshot is the previous draw's.
	const bool shaders_same     = state.shaders.Same(shaders, shaders_size);
	const bool user_config_same = state.user_config.Same(user_config, user_config_size);
	state.kind = !context_same                       ? Changed
	             : shaders_same && user_config_same ? AllSame
	                                                : ContextSame;
	state.stats[state.kind].draws++;
	state.open = true;

	const auto kept       = std::min<uint32_t>(state.block_count, BlocksKept);
	const bool same_list  = state.block_count == state.last_block_count &&
	                       std::equal(state.blocks.begin(), state.blocks.begin() + kept,
	                                  state.last_blocks.begin());
	state.blocks_total += state.block_count;
	// A draw with no blocks of its own repeats nothing: only a non-empty list is counted.
	if (same_list && state.block_count != 0) {
		state.blocks_repeated++;
		if (!context_same) {
			state.blocks_but_changed++;
		}
	} else if (context_same && state.block_count != 0) {
		state.same_other_blocks++;
	}
	if (state.block_count != 0) {
		state.last_blocks      = state.blocks;
		state.last_block_count = state.block_count;
	}
	state.block_count = 0;

	state.draws++;
	ReportIfDue(state);
}

void NoteStep(Step step, uint64_t signature, uint64_t cycles) {
	if (!Enabled()) {
		return;
	}
	auto& state = GetState();
	if (!state.open) {
		return;
	}
	const auto index = static_cast<size_t>(step);
	auto&      stats = state.stats[state.kind];
	stats.count[index]++;
	stats.cycles[index] += cycles;
	if (state.last_signature[index] == signature) {
		stats.same[index]++;
	}
	state.last_signature[index] = signature;
}

} // namespace Libs::Graphics::StateRepeat
