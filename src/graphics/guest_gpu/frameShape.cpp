#include "graphics/guest_gpu/frameShape.h"

#include "common/logging/log.h"
#include "common/perfStats.h"
#include "graphics/guest_gpu/pm4.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

namespace Libs::Graphics::FrameShape {

namespace {

bool EnvOn(const char* name) {
	const char* value = std::getenv(name);
	return value != nullptr && value[0] == '1';
}

// The key state a draw or a dispatch reads.
enum Key : uint32_t { PsProgram, GsProgram, ColorTarget, DepthTarget, Viewport, CsProgram, Keys };

constexpr std::array<const char*, Keys> KeyNames = {"PS program", "GS program", "color target 0",
                                                    "depth target",
                                                    "viewport",   "CS program"};

// Runs and submissions, by length: 1, 2-3, 4-15, 16-63, 64-255, 256 or more.
constexpr uint32_t Buckets = 6;
constexpr std::array<const char*, Buckets> BucketNames = {"1", "2-3", "4-15", "16-63", "64-255",
                                                          "256+"};

uint32_t Bucket(uint64_t n) {
	return n <= 1 ? 0 : n <= 3 ? 1 : n <= 15 ? 2 : n <= 63 ? 3 : n <= 255 ? 4 : 5;
}

struct ProcessorState {
	std::bitset<Pm4::CX_NUM> cx_submission, cx_buffer;
	std::bitset<Pm4::SH_NUM> sh_submission, sh_buffer;
	bool     graphics    = true;
	bool     in_buffer   = false;
	uint64_t draws        = 0; // this submission's
	uint64_t buffer_draws = 0; // this indirect buffer's (or the submission's before any)
	uint64_t dispatches  = 0;
	uint64_t color_base  = 0;
	uint64_t depth_base  = 0;
	uint64_t run_target  = UINT64_MAX; // the target of the current run (color ^ depth << 1)
	uint64_t run_draws   = 0;
	uint64_t chunk_commands = 0; // draws and dispatches since the last packet with effects
	bool     seen_submit = false;
};

struct Totals {
	uint64_t submissions[2] {}; // compute, graphics
	uint64_t empty_submissions[2] {};
	uint64_t buffers = 0, chained = 0;
	uint64_t draws = 0, dispatches = 0;
	uint64_t target_runs = 0;
	// Draws by the length of the submission they are in, and of their render target run;
	// dispatches by their submission's dispatch count.
	std::array<uint64_t, Buckets> draws_by_submission {}, draws_by_run {}, draws_by_buffer {},
	    dispatches_by_submission {};
	// Draws (dispatches for the CS program) whose key register no packet of their submission
	// wrote / of their indirect buffer (draws in one).
	std::array<uint64_t, Keys> inherited_submission {}, inherited_buffer {};
	// Slice 1: chunks of pure packets, commands by their chunk's length, and what ended them
	// (opcode << 8 | custom code: chunks ended, commands in them).
	uint64_t                                                 chunks = 0;
	uint64_t graphics_chunks = 0, compute_chunks = 0;
	uint64_t graphics_chunk_commands = 0, compute_chunk_commands = 0;
	std::array<uint64_t, Buckets>                            commands_by_chunk {};
	std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> enders;
	uint64_t                   in_buffer = 0, dispatches_in_buffer = 0;
	uint32_t                   first_frame = 0, last_frame = 0;
	std::chrono::steady_clock::time_point last {};
};

std::unordered_map<const void*, ProcessorState> g_states;
Totals                                          g_totals;

ProcessorState& State(const void* processor) {
	return g_states[processor];
}

void EndRun(ProcessorState& s) {
	if (s.run_draws != 0) {
		g_totals.target_runs++;
		g_totals.draws_by_run[Bucket(s.run_draws)] += s.run_draws;
	}
	s.run_draws  = 0;
	s.run_target = UINT64_MAX;
}

void EndSubmission(ProcessorState& s) {
	if (!s.seen_submit) {
		return;
	}
	EndRun(s);
	if (s.buffer_draws != 0) {
		g_totals.draws_by_buffer[Bucket(s.buffer_draws)] += s.buffer_draws;
	}
	s.buffer_draws = 0;
	if (s.draws != 0) {
		g_totals.draws_by_submission[Bucket(s.draws)] += s.draws;
	}
	if (s.dispatches != 0) {
		g_totals.dispatches_by_submission[Bucket(s.dispatches)] += s.dispatches;
	}
	if (s.draws == 0 && s.dispatches == 0) {
		g_totals.empty_submissions[s.graphics ? 1 : 0]++;
	}
	s.draws      = 0;
	s.dispatches = 0;
}

std::string Shares(const std::array<uint64_t, Buckets>& counts) {
	uint64_t total = 0;
	for (auto c: counts) {
		total += c;
	}
	std::string out;
	for (uint32_t i = 0; i < Buckets; i++) {
		out += fmt::format("{}{} {:.0f}%", i == 0 ? "" : ", ", BucketNames[i],
		                   total == 0 ? 0.0 : 100.0 * static_cast<double>(counts[i]) /
		                                          static_cast<double>(total));
	}
	return out;
}

std::string Enders(const Totals& t) {
	std::vector<std::pair<uint32_t, std::pair<uint64_t, uint64_t>>> list(t.enders.begin(),
	                                                                     t.enders.end());
	std::sort(list.begin(), list.end(),
	          [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
	std::string out;
	for (size_t i = 0; i < list.size() && i < 12; i++) {
		const auto key = list[i].first;
		out += fmt::format("{}{:02x}{} {} ({} commands)", i == 0 ? "" : ", ", key >> 8u,
		                   (key & 0xffu) != 0 ? fmt::format("/r{:02x}", key & 0xffu) : "",
		                   list[i].second.first, list[i].second.second);
	}
	return out;
}

void ReportIfDue(uint32_t frame) {
	auto&      t   = g_totals;
	const auto now = std::chrono::steady_clock::now();
	if (t.last.time_since_epoch().count() == 0) {
		t             = {};
		t.last        = now;
		t.first_frame = frame;
		return;
	}
	t.last_frame = frame;
	if (now - t.last < std::chrono::seconds(10)) {
		return;
	}
	const double frames = std::max(1.0, static_cast<double>(t.last_frame - t.first_frame));
	const double draws  = std::max<double>(1.0, static_cast<double>(t.draws));
	const double disp   = std::max<double>(1.0, static_cast<double>(t.dispatches));
	std::string  inherited;
	for (uint32_t k = 0; k < Keys; k++) {
		const double base = k == CsProgram ? disp : draws;
		inherited += fmt::format(
		    "{}{} {:.1f}% ({:.1f}% of those in a buffer)", k == 0 ? "" : ", ", KeyNames[k],
		    100.0 * static_cast<double>(t.inherited_submission[k]) / base,
		    100.0 * static_cast<double>(t.inherited_buffer[k]) /
		        std::max<double>(1.0, static_cast<double>(k == CsProgram ? t.dispatches_in_buffer
		                                                                 : t.in_buffer)));
	}
	Log::WriteUnmuted(fmt::format(
	    "Frame shape ({:.0f} frames{}): a frame: {:.1f} graphics submissions ({:.1f} empty), "
	    "{:.1f} compute ({:.1f} empty), {:.1f} indirect buffers + {:.1f} chained, {:.0f} draws "
	    "({:.1f}% in an indirect buffer), {:.0f} dispatches, {:.0f} render target runs | draws by "
	    "their submission's draws: {} | by their buffer's (chained pieces apart): {} | by their "
	    "target run: {} | dispatches by their submission's: {} "
	    "| key state not written in the submission: {}\n",
	    frames, NullRender() ? ", null render" : "",
	    static_cast<double>(t.submissions[1]) / frames,
	    static_cast<double>(t.empty_submissions[1]) / frames,
	    static_cast<double>(t.submissions[0]) / frames,
	    static_cast<double>(t.empty_submissions[0]) / frames,
	    static_cast<double>(t.buffers) / frames, static_cast<double>(t.chained) / frames,
	    static_cast<double>(t.draws) / frames,
	    100.0 * static_cast<double>(t.in_buffer) / draws,
	    static_cast<double>(t.dispatches) / frames, static_cast<double>(t.target_runs) / frames,
	    Shares(t.draws_by_submission), Shares(t.draws_by_buffer), Shares(t.draws_by_run), Shares(t.dispatches_by_submission),
	    inherited));
	Log::WriteUnmuted(fmt::format(
	    "Chunks ({:.0f} frames): {:.1f} a frame with commands (graphics {:.1f} of {:.1f} commands "
	    "each, compute {:.1f} of {:.1f}) | commands by their chunk's: {} | "
	    "ended by (opcode/custom code: chunks with commands, their commands): {}\n",
	    frames, static_cast<double>(t.chunks) / frames,
	    static_cast<double>(t.graphics_chunks) / frames,
	    static_cast<double>(t.graphics_chunk_commands) /
	        std::max<double>(1.0, static_cast<double>(t.graphics_chunks)),
	    static_cast<double>(t.compute_chunks) / frames,
	    static_cast<double>(t.compute_chunk_commands) /
	        std::max<double>(1.0, static_cast<double>(t.compute_chunks)),
	    Shares(t.commands_by_chunk), Enders(t)));
	t             = {};
	t.last        = now;
	t.first_frame = frame;
}

void CountInherited(const ProcessorState& s, Key key, bool written_submission,
                    bool written_buffer) {
	if (!written_submission) {
		g_totals.inherited_submission[key]++;
	}
	if (s.in_buffer && !written_buffer) {
		g_totals.inherited_buffer[key]++;
	}
}

} // namespace

bool Enabled() {
	static const bool on = EnvOn("KYTY_FRAME_SHAPE");
	return on;
}

bool NullRender() {
	// KYTY_NULL_RENDER=file: on while a file named _null-render is in the working directory
	// (looked at twice a second), so a race can be started with the picture and the null mode
	// switched on while driving.
	static const char* const mode = std::getenv("KYTY_NULL_RENDER");
	if (mode == nullptr) {
		return false;
	}
	if (mode[0] == '1') {
		return true;
	}
	if (std::string_view(mode) != "file") {
		return false;
	}
	static bool                                  on = false;
	static std::chrono::steady_clock::time_point checked {};
	const auto                                   now = std::chrono::steady_clock::now();
	if (now - checked >= std::chrono::milliseconds(500)) {
		checked = now;
		std::error_code error;
		on      = std::filesystem::exists("_null-render", error);
	}
	return on;
}

void OnSubmission(const void* processor, bool graphics, uint32_t frame) {
	auto& s = State(processor);
	EndSubmission(s);
	s.seen_submit = true;
	s.graphics    = graphics;
	s.in_buffer   = false;
	s.cx_submission.reset();
	s.sh_submission.reset();
	s.cx_buffer.reset();
	s.sh_buffer.reset();
	g_totals.submissions[graphics ? 1 : 0]++;
	ReportIfDue(frame);
}

void OnIndirectBuffer(const void* processor, bool chain) {
	auto& s     = State(processor);
	s.in_buffer = true;
	s.cx_buffer.reset();
	s.sh_buffer.reset();
	(chain ? g_totals.chained : g_totals.buffers)++;
	if (s.buffer_draws != 0) {
		g_totals.draws_by_buffer[Bucket(s.buffer_draws)] += s.buffer_draws;
	}
	s.buffer_draws = 0;
}

void NoteCx(const void* processor, uint32_t reg, uint32_t value) {
	if (reg >= Pm4::CX_NUM) {
		return;
	}
	auto& s = State(processor);
	s.cx_submission.set(reg);
	s.cx_buffer.set(reg);
	if (reg == Pm4::CB_COLOR0_BASE) {
		s.color_base = value;
	} else if (reg == Pm4::DB_Z_READ_BASE) {
		s.depth_base = value;
	}
}

void NoteSh(const void* processor, uint32_t reg, uint32_t /*value*/) {
	if (reg >= Pm4::SH_NUM) {
		return;
	}
	auto& s = State(processor);
	s.sh_submission.set(reg);
	s.sh_buffer.set(reg);
}

void OnDraw(const void* processor) {
	auto& s = State(processor);
	s.draws++;
	s.buffer_draws++;
	s.chunk_commands++;
	g_totals.draws++;
	g_totals.in_buffer += s.in_buffer ? 1 : 0;
	const uint64_t target = s.color_base ^ (s.depth_base << 1u);
	if (target != s.run_target) {
		EndRun(s);
		s.run_target = target;
	}
	s.run_draws++;
	CountInherited(s, PsProgram, s.sh_submission[Pm4::SPI_SHADER_PGM_LO_PS],
	               s.sh_buffer[Pm4::SPI_SHADER_PGM_LO_PS]);
	// A GS binary's program is at SPI_SHADER_PGM_LO_ES (Agc's GetProgramOffset).
	CountInherited(s, GsProgram,
	               s.sh_submission[Pm4::SPI_SHADER_PGM_LO_ES] ||
	                   s.sh_submission[Pm4::SPI_SHADER_PGM_LO_GS],
	               s.sh_buffer[Pm4::SPI_SHADER_PGM_LO_ES] || s.sh_buffer[Pm4::SPI_SHADER_PGM_LO_GS]);
	CountInherited(s, ColorTarget, s.cx_submission[Pm4::CB_COLOR0_BASE],
	               s.cx_buffer[Pm4::CB_COLOR0_BASE]);
	CountInherited(s, DepthTarget, s.cx_submission[Pm4::DB_Z_READ_BASE],
	               s.cx_buffer[Pm4::DB_Z_READ_BASE]);
	CountInherited(s, Viewport, s.cx_submission[Pm4::PA_CL_VPORT_XSCALE],
	               s.cx_buffer[Pm4::PA_CL_VPORT_XSCALE]);
}

bool IsPurePacket(uint32_t opcode, uint32_t custom) {
	switch (opcode) {
		case Pm4::IT_SET_SH_REG_INDIRECT:
		case Pm4::IT_SET_UCONFIG_REG_INDIRECT:
		case Pm4::IT_SET_CONFIG_REG:
		case Pm4::IT_SET_CONTEXT_REG:
		case Pm4::IT_SET_SH_REG:
		case Pm4::IT_SET_UCONFIG_REG:
		case Pm4::IT_SET_UCONFIG_REG_INDEX:
		case Pm4::IT_SET_CONTEXT_REG_INDIRECT:
		case Pm4::IT_INDEX_TYPE:
		case Pm4::IT_INDEX_BASE:
		case Pm4::IT_INDEX_BUFFER_SIZE:
		case Pm4::IT_NUM_INSTANCES:
		case Pm4::IT_SET_BASE:
		case Pm4::IT_INDIRECT_BUFFER: return true;
		case Pm4::IT_NOP:
			return custom == Pm4::R_ZERO || custom == Pm4::R_PUSH_MARKER ||
			       custom == Pm4::R_POP_MARKER || custom == Pm4::R_DRAW_RESET ||
			       custom == Pm4::R_DISPATCH_RESET || custom == Pm4::R_CONTEXT_STATE ||
			       IsCommandPacket(opcode, custom);
		default: return IsCommandPacket(opcode, custom);
	}
}

bool IsSoftPacket(uint32_t opcode, uint32_t custom) {
	return opcode == Pm4::IT_EVENT_WRITE || opcode == Pm4::IT_RELEASE_MEM ||
	       opcode == Pm4::IT_ACQUIRE_MEM ||
	       (opcode == Pm4::IT_NOP &&
	        (custom == Pm4::R_RELEASE_MEM || custom == Pm4::R_ACQUIRE_MEM));
}

bool IsCommandPacket(uint32_t opcode, uint32_t custom) {
	switch (opcode) {
		case Pm4::IT_DRAW_INDIRECT:
		case Pm4::IT_DRAW_INDEX_INDIRECT:
		case Pm4::IT_DRAW_INDEX_2:
		case Pm4::IT_DRAW_INDIRECT_MULTI:
		case Pm4::IT_DRAW_INDEX_AUTO:
		case Pm4::IT_DRAW_INDEX_OFFSET_2:
		case Pm4::IT_DRAW_INDEX_INDIRECT_MULTI:
		case Pm4::IT_DISPATCH_DIRECT:
		case Pm4::IT_DISPATCH_INDIRECT: return true;
		case Pm4::IT_NOP: return custom == Pm4::R_GT7_DRAW;
		default: return false;
	}
}

void OnPacket(const void* processor, uint32_t opcode, uint32_t custom) {
	// With KYTY_REPLAY_SOFT=1 the chunks the replay sees: releases and event writes inside them.
	static const bool soft_inside = [] {
		const char* value = std::getenv("KYTY_REPLAY_SOFT");
		return value != nullptr && value[0] == '1';
	}();
	const bool pure = IsPurePacket(opcode, custom) || (soft_inside && IsSoftPacket(opcode, custom));
	if (pure) {
		return;
	}
	auto& s = State(processor);
	if (s.chunk_commands != 0) {
		g_totals.chunks++;
		(s.graphics ? g_totals.graphics_chunks : g_totals.compute_chunks)++;
		(s.graphics ? g_totals.graphics_chunk_commands : g_totals.compute_chunk_commands) +=
		    s.chunk_commands;
		g_totals.commands_by_chunk[Bucket(s.chunk_commands)] += s.chunk_commands;
		auto& ender = g_totals.enders[opcode << 8u | (opcode == Pm4::IT_NOP ? custom : 0u)];
		ender.first++;
		ender.second += s.chunk_commands;
	}
	s.chunk_commands = 0;
}

void OnDispatch(const void* processor) {
	auto& s = State(processor);
	s.dispatches++;
	s.chunk_commands++;
	g_totals.dispatches++;
	g_totals.dispatches_in_buffer += s.in_buffer ? 1 : 0;
	CountInherited(s, CsProgram, s.sh_submission[Pm4::COMPUTE_PGM_LO],
	               s.sh_buffer[Pm4::COMPUTE_PGM_LO]);
}

} // namespace Libs::Graphics::FrameShape
