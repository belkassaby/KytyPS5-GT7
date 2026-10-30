#include "loader/gt7NativeDraws.h"

#include "common/abi.h"
#include "common/logging/log.h"
#include "common/perfStats.h"
#include "common/virtualMemory.h"
#include "graphics/guest_gpu/pm4.h"
#include "kernel/memory.h"
#include "graphics/host_gpu/renderer/drawMutations.h"
#include "loader/gt7GuestHook.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

namespace Loader::Gt7NativeDraws {

namespace Pm4 = Libs::Graphics::Pm4;

namespace {

constexpr uint64_t BindMaterial     = 0x11800; // gt7_BindMaterial(set, material, package, shaders, ...)
constexpr uint64_t MeshDraw         = 0x5fb0;  // gt7_DrawMeshClusters(mesh, ...)
constexpr uint64_t MeshDrawSize     = 9124;
constexpr uint64_t DrawIndex        = 0xa1400; // gt7_DrawIndex
constexpr uint64_t DrawIndexSize    = 434;
constexpr uint64_t MaterialBlock    = 0x18; // GT7Material's constant block
constexpr uint64_t CommitDrawState  = 0x16370; // gt7_CommitDrawState(ctx)
constexpr auto     ReportEvery      = std::chrono::seconds(10);

// push rbp; mov rbp, rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp, 0x138
constexpr std::array<uint8_t, 20> BindMoved = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                               0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                               0x81, 0xec, 0x38, 0x01, 0x00, 0x00};
// the same with sub rsp, 0x6e8
constexpr std::array<uint8_t, 20> MeshMoved = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                               0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                               0x81, 0xec, 0xe8, 0x06, 0x00, 0x00};

// push rbp; mov rbp, rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp, 0x158
constexpr std::array<uint8_t, 20> CommitMoved = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                                 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                                 0x81, 0xec, 0x58, 0x01, 0x00, 0x00};

// GT7RenderContext (docs/GT7-RENDER-FRONTEND.md): what the N1 state key covers. The colour and
// depth targets, the fields from the index state to the attribute slots (fixed-function state
// the dirty groups commit, waits and predication excluded below), each stage's shader, the
// primitive type and the vertex formats.
constexpr uint64_t CtxTargets      = 0x10;
constexpr uint64_t CtxDepthTarget  = 0x418;
constexpr uint64_t CtxTargetsEnd   = 0x718;
// N2: the dynamic state among them (gt7_CommitDrawState's groups 3, 12 and 16), which Vulkan
// sets per draw and the pipeline key leaves out: 16 viewports of 0x1c bytes (+0x68c: the mask
// of those used), the screen scissor rectangles, the polygon offset values.
constexpr uint64_t CtxViewports     = 0x4cc;
constexpr uint64_t CtxViewportsEnd  = 0x68c;
constexpr uint64_t CtxScissors      = 0x69c;
constexpr uint64_t CtxScissorsEnd   = 0x6b0;
constexpr uint64_t CtxPolyOffset    = 0x6f0;
constexpr uint64_t CtxAttribSlots  = 0x770;
constexpr uint64_t CtxAttribEnd    = 0x788;
constexpr uint64_t CtxStages       = 0x870;
constexpr uint64_t CtxStageSize    = 0xb0;
constexpr uint64_t CtxPrimType     = 0xc94;
constexpr uint64_t CtxVertexFormat = 0xda4;
constexpr uint64_t CtxVertexEnd    = 0xde8;

bool     g_enabled   = false;
uint64_t g_main_base = 0;

// Game threads: the engine state the next draw marker carries.
thread_local Identity t_identity;

// GPU thread: the marker waiting for its draw.
bool     g_pending = false;
Identity g_pending_identity;

std::atomic<uint64_t> g_markers_written {0};
std::atomic<uint64_t> g_markers {0};        // seen by the command processor
std::atomic<uint64_t> g_orphans {0};        // replaced by another marker before a draw
std::atomic<uint64_t> g_draws {0};          // indexed draws
std::atomic<uint64_t> g_marked {0};         // ... with a marker
std::atomic<uint64_t> g_block_match {0};    // ... whose pixel stage's SGPRs 2-3 are the block
std::atomic<uint64_t> g_no_block {0};       // ... whose material had no block
std::atomic<uint64_t> g_with_mesh {0};
std::unordered_set<uint64_t> g_materials;   // GPU thread only
std::atomic<uint64_t>        g_material_count {0};

// Milestone 2 (GPU thread): the material of the draw being executed, when its pixel stage's
// texture table is the material's block (0 otherwise), and the pixel-stage binding counts.
uint64_t              g_current_material = 0;
std::atomic<uint64_t> g_binding_draws {0};
std::atomic<uint64_t> g_binding_hits {0};
std::atomic<uint64_t> g_binding_ns {0};
std::atomic<uint64_t> g_binding_mismatches {0};
std::atomic<uint64_t> g_binding_uncacheable {0};

// Milestone 3, measured first (GPU thread): the marked draw being executed, its template keys
// (material, mesh, programs, pipeline) and when each was last seen, and the renderer's time.
bool                                   g_current_marked = false;
Identity                               g_current_identity;
std::unordered_map<uint64_t, uint64_t> g_template_frames; // key -> the frame it was last seen
uint64_t                               g_template_frame = 0;
std::unordered_set<uint64_t>           g_template_frame_keys; // this frame's
std::atomic<uint64_t>                  g_template_notes {0};
std::atomic<uint64_t>                  g_template_previous {0}; // seen the frame before
std::atomic<uint64_t>                  g_template_same {0};     // seen earlier this frame
std::atomic<uint64_t>                  g_template_older {0};
std::atomic<uint64_t>                  g_template_new {0};
std::atomic<uint64_t>                  g_template_distinct {0}; // summed over frames
std::atomic<uint64_t>                  g_marked_draw_ns {0};
std::atomic<uint64_t>                  g_marked_draw_count {0};
std::atomic<uint64_t>                  g_other_draw_ns {0};
std::atomic<uint64_t>                  g_other_draw_count {0};

template <typename T>
T Read(uint64_t address) {
	T value {};
	std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
	return value;
}

KYTY_SYSV_ABI void OnBindMaterial(uint64_t set, uint64_t material, uint64_t /*package*/,
                                  uint64_t shaders) {
	t_identity.set      = set;
	t_identity.material = material;
	t_identity.block    = material != 0 ? Read<uint64_t>(material + MaterialBlock) : 0;
	t_identity.shaders  = shaders;
}

KYTY_SYSV_ABI void OnMeshDraw(uint64_t mesh) {
	t_identity.mesh = mesh;
}

// N1: the render thread's state key, made at every commit (the draw follows it).
std::atomic<uint64_t> g_state_keys {0};
std::atomic<uint64_t> g_state_ns {0};

uint64_t Mix(uint64_t key, uint64_t value) {
	return (key ^ value) * 0x9E3779B97F4A7C15ull + (key >> 29u);
}

uint64_t HashBytes(uint64_t key, const uint8_t* data, size_t size) {
	size_t i = 0;
	for (; i + 8 <= size; i += 8) {
		uint64_t word = 0;
		std::memcpy(&word, data + i, 8);
		key = Mix(key, word);
	}
	for (; i < size; i++) {
		key = Mix(key, data[i]);
	}
	return key;
}

// N2, measured first: the key's parts, on a sample of the commits (one in 16 a thread): how often
// each part's value is one not seen before, and the 32-bit fields of the hashed regions with the
// most distinct values (what varies from draw to draw). A "Native key parts" line every 10 s.
enum KeyPart : uint32_t { PartColor, PartDepth, PartFixed, PartAttribs, PartShaders, PartPrim,
                          PartFormats, PartAll, PartCount };
constexpr std::array<const char*, PartCount> PartNames = {
    "colour targets", "depth target", "fixed function", "attribute slots", "shaders",
    "primitive type", "vertex formats", "all"};
struct KeyParts {
	std::mutex                                                  mutex;
	std::array<std::unordered_set<uint64_t>, PartCount>         seen;
	std::array<uint64_t, PartCount>                             fresh {};
	uint64_t                                                    samples = 0;
	// distinct values by field offset (capped)
	std::unordered_map<uint32_t, std::unordered_set<uint32_t>> fields;
};
KeyParts g_key_parts;
thread_local uint32_t t_key_sample = 0;

void SampleKeyParts(const uint8_t* ctx, const std::array<uint64_t, PartCount>& parts) {
	auto&           k = g_key_parts;
	std::lock_guard lock(k.mutex);
	k.samples++;
	for (uint32_t i = 0; i < PartCount; i++) {
		if (k.seen[i].size() > (1u << 18u)) {
			k.seen[i].clear();
		}
		if (k.seen[i].insert(parts[i]).second) {
			k.fresh[i]++;
		}
	}
	const auto fields = [&](uint64_t begin, uint64_t end) {
		for (uint64_t offset = begin; offset + 4 <= end; offset += 4) {
			uint32_t value = 0;
			std::memcpy(&value, ctx + offset, sizeof(value));
			auto& set = k.fields[static_cast<uint32_t>(offset)];
			if (set.size() < 4096) {
				set.insert(value);
			}
		}
	};
	fields(CtxTargets, CtxTargetsEnd);
	fields(CtxAttribSlots, CtxAttribEnd);
	fields(CtxVertexFormat, CtxVertexEnd);
}

void ReportKeyParts() {
	auto&           k = g_key_parts;
	std::lock_guard lock(k.mutex);
	if (k.samples == 0) {
		return;
	}
	const double samples = static_cast<double>(k.samples);
	std::string  parts;
	for (uint32_t i = 0; i < PartCount; i++) {
		parts += fmt::format("{}{} {:.1f}%", i == 0 ? "" : ", ", PartNames[i],
		                     100.0 * static_cast<double>(k.fresh[i]) / samples);
	}
	std::vector<std::pair<uint32_t, size_t>> varied;
	for (const auto& [offset, set]: k.fields) {
		if (set.size() > 1) {
			varied.emplace_back(offset, set.size());
		}
	}
	std::sort(varied.begin(), varied.end(),
	          [](const auto& a, const auto& b) { return a.second > b.second; });
	std::string fields;
	for (size_t i = 0; i < varied.size() && i < 24; i++) {
		fields += fmt::format("{}+{:#x} {}", i == 0 ? "" : ", ", varied[i].first, varied[i].second);
	}
	Log::WriteUnmuted(fmt::format(
	    "Native key parts (10 s, {} sampled commits): new values: {} | fields with the most "
	    "distinct values (offset, values): {}\n",
	    k.samples, parts, fields));
	k.samples = 0;
	k.fresh   = {};
	k.fields.clear();
}

KYTY_SYSV_ABI void OnCommitState(const uint8_t* ctx) {
	const auto start = std::chrono::steady_clock::now();
	constexpr uint64_t Seed = 0x6a09e667f3bcc909ull;
	std::array<uint64_t, PartCount> parts {};
	parts[PartColor] = HashBytes(Seed, ctx + CtxTargets, CtxDepthTarget - CtxTargets);
	parts[PartDepth] = HashBytes(Seed, ctx + CtxDepthTarget, CtxViewports - CtxDepthTarget);
	parts[PartFixed] = HashBytes(Seed, ctx + CtxViewportsEnd, CtxScissors - CtxViewportsEnd);
	parts[PartFixed] =
	    HashBytes(parts[PartFixed], ctx + CtxScissorsEnd, CtxPolyOffset - CtxScissorsEnd);
	parts[PartAttribs] = HashBytes(Seed, ctx + CtxAttribSlots, CtxAttribEnd - CtxAttribSlots);
	parts[PartShaders] = Seed;
	for (uint64_t stage = 0; stage < 6; stage++) {
		uint64_t shader = 0;
		std::memcpy(&shader, ctx + CtxStages + stage * CtxStageSize, sizeof(shader));
		parts[PartShaders] = Mix(parts[PartShaders], shader);
	}
	uint32_t prim = 0;
	std::memcpy(&prim, ctx + CtxPrimType, sizeof(prim));
	parts[PartPrim]    = Mix(Seed, prim);
	parts[PartFormats] = HashBytes(Seed, ctx + CtxVertexFormat, CtxVertexEnd - CtxVertexFormat);
	uint64_t key       = Seed;
	for (uint32_t i = 0; i < PartAll; i++) {
		key = Mix(key, parts[i]);
	}
	parts[PartAll]   = key;
	t_identity.state = key | 1u; // never 0
	if ((t_key_sample++ & 15u) == 0) {
		SampleKeyParts(ctx, parts);
	}
	g_state_keys.fetch_add(1, std::memory_order_relaxed);
	g_state_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                               std::chrono::steady_clock::now() - start)
	                                               .count()),
	                     std::memory_order_relaxed);
}

// N1, GPU thread (NoteTemplate): what Kyty's translation chose for each key. By the state key
// alone, and by the state key with the material (the programs depend on the bound textures).
struct StateChoice {
	uint64_t programs = 0;
	uint64_t pipeline = 0;
};
std::unordered_map<uint64_t, StateChoice> g_state_choices;
std::unordered_map<uint64_t, StateChoice> g_state_material_choices;
std::atomic<uint64_t> g_n1_draws {0}, g_n1_no_key {0}, g_n1_new {0}, g_n1_same {0};
std::atomic<uint64_t> g_n1_programs_differ {0}, g_n1_pipeline_differ {0};
std::atomic<uint64_t> g_n1_m_new {0}, g_n1_m_same {0}, g_n1_m_programs_differ {0},
    g_n1_m_pipeline_differ {0};

void NoteStateChoice(uint64_t state, uint64_t material, uint64_t programs, uint64_t pipeline) {
	g_n1_draws.fetch_add(1, std::memory_order_relaxed);
	if (state == 0) {
		g_n1_no_key.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	const auto note = [&](std::unordered_map<uint64_t, StateChoice>& map, uint64_t key,
	                      std::atomic<uint64_t>& fresh, std::atomic<uint64_t>& same,
	                      std::atomic<uint64_t>& programs_differ,
	                      std::atomic<uint64_t>& pipeline_differ) {
		if (map.size() > (1u << 20u)) {
			map.clear();
		}
		auto [it, inserted] = map.try_emplace(key, StateChoice {programs, pipeline});
		if (inserted) {
			fresh.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		const bool p = it->second.programs != programs;
		const bool q = it->second.pipeline != pipeline;
		if (!p && !q) {
			same.fetch_add(1, std::memory_order_relaxed);
		}
		if (p) {
			programs_differ.fetch_add(1, std::memory_order_relaxed);
		}
		if (q) {
			pipeline_differ.fetch_add(1, std::memory_order_relaxed);
		}
		it->second = {programs, pipeline};
	};
	note(g_state_choices, state, g_n1_new, g_n1_same, g_n1_programs_differ, g_n1_pipeline_differ);
	note(g_state_material_choices, Mix(state, material), g_n1_m_new, g_n1_m_same,
	     g_n1_m_programs_differ, g_n1_m_pipeline_differ);
}

void ReportStateKeys() {
	static std::array<uint64_t, 12> last {};
	const std::array<uint64_t, 12>  now = {
        g_state_keys.load(),       g_state_ns.load(),          g_n1_draws.load(),
        g_n1_no_key.load(),        g_n1_new.load(),            g_n1_same.load(),
        g_n1_programs_differ.load(), g_n1_pipeline_differ.load(), g_n1_m_new.load(),
        g_n1_m_same.load(),        g_n1_m_programs_differ.load(), g_n1_m_pipeline_differ.load()};
	std::array<double, 12> d {};
	for (size_t i = 0; i < now.size(); i++) {
		d[i]    = static_cast<double>(now[i] - last[i]);
		last[i] = now[i];
	}
	const double keyed = std::max(1.0, d[2] - d[3]);
	Log::WriteUnmuted(fmt::format(
	    "Native records N1 (10 s): {:.0f} commits keyed on the render threads, {:.2f} us each | "
	    "{:.0f} marked draws, {:.0f} without a key | by the state key: {:.1f}% new, {:.1f}% the "
	    "same programs and pipeline as before, {:.2f}% other programs, {:.2f}% another pipeline | "
	    "by state and material: {:.1f}% new, {:.1f}% the same, {:.2f}% other programs, {:.2f}% "
	    "another pipeline\n",
	    d[0], d[0] == 0 ? 0.0 : d[1] / 1000.0 / d[0], d[2], d[3], 100.0 * d[4] / keyed,
	    100.0 * d[5] / keyed, 100.0 * d[6] / keyed, 100.0 * d[7] / keyed, 100.0 * d[8] / keyed,
	    100.0 * d[9] / keyed, 100.0 * d[10] / keyed, 100.0 * d[11] / keyed));
}

// Milestone 4: what marked draws wrote in the renderer's shared state
// (Libs::Graphics::DrawMutations), by kind and phase (prepare, record), and how many wrote nothing
namespace Mutations = Libs::Graphics::DrawMutations;
std::array<std::array<std::atomic<uint64_t>, Mutations::Count>, 2> g_mutation_counts {};
std::atomic<uint64_t> g_mutation_draws {0};
std::atomic<uint64_t> g_prepare_pure {0}; // nothing written while preparing
std::atomic<uint64_t> g_record_pure {0};  // nothing written while recording
std::atomic<uint64_t> g_pure_draws {0};   // neither
std::atomic<uint64_t> g_pure_ns {0};
std::atomic<uint64_t> g_impure_ns {0};

void ReportMutations() {
	static std::array<std::array<uint64_t, Mutations::Count>, 2> last {};
	static uint64_t last_draws = 0, last_prepare = 0, last_record = 0, last_pure = 0,
	                last_pure_ns = 0, last_impure_ns = 0;
	const auto draws = g_mutation_draws.load(), pure = g_pure_draws.load();
	const auto prepare = g_prepare_pure.load(), record = g_record_pure.load();
	const auto pure_ns = g_pure_ns.load(), impure_ns = g_impure_ns.load();
	const auto n       = static_cast<double>(std::max<uint64_t>(draws - last_draws, 1));
	const auto p       = static_cast<double>(pure - last_pure);
	const char* names[Mutations::Count] = {"barrier",      "render pass",   "new image",
	                                       "image upload", "new buffer",    "buffer upload",
	                                       "new pipeline", "program entry", "texture desc",
	                                       "texture memo", "new sampler"};
	std::string kinds[2];
	for (uint32_t phase = 0; phase < 2; phase++) {
		for (uint32_t i = 0; i < Mutations::Count; i++) {
			const auto now = g_mutation_counts[phase][i].load();
			const auto pct = 100.0 * static_cast<double>(now - last[phase][i]) / n;
			if (pct >= 0.05) {
				kinds[phase] += fmt::format(" {} {:.1f}%", names[i], pct);
			}
			last[phase][i] = now;
		}
	}
	Log::WriteUnmuted(fmt::format(
	    "Native mutations: {:.0f} marked draws: {:.1f}% wrote no shared state while preparing, "
	    "{:.1f}% while recording, {:.1f}% at all ({:.2f} us each in the renderer; the others {:.2f} "
	    "us) | preparing:{} | recording:{}\n",
	    n, 100.0 * static_cast<double>(prepare - last_prepare) / n,
	    100.0 * static_cast<double>(record - last_record) / n, 100.0 * p / n,
	    p == 0 ? 0.0 : static_cast<double>(pure_ns - last_pure_ns) / 1000.0 / p,
	    n - p == 0 ? 0.0 : static_cast<double>(impure_ns - last_impure_ns) / 1000.0 / (n - p),
	    kinds[0], kinds[1]));
	last_draws     = draws;
	last_prepare   = prepare;
	last_record    = record;
	last_pure      = pure;
	last_pure_ns   = pure_ns;
	last_impure_ns = impure_ns;
}

// CommitBindings' steps for marked draws (AddCommitSteps)
std::array<std::atomic<uint64_t>, 5> g_commit_ns {};
std::atomic<uint64_t>                g_commit_calls {0};

// ResolveTexture's steps for marked draws' pixel stages (AddResolveSteps)
std::array<std::atomic<uint64_t>, 8> g_resolve {}; // key, memo, slow, bind ns; images, hits, slows, stages

void ReportResolve() {
	static std::array<uint64_t, 8> last {};
	double d[8];
	for (int i = 0; i < 8; i++) {
		const auto now = g_resolve[i].load();
		d[i]           = static_cast<double>(now - last[i]);
		last[i]        = now;
	}
	const auto stages = std::max(d[7], 1.0);
	Log::WriteUnmuted(fmt::format(
	    "Native resolve: {:.0f} marked pixel stages, {:.1f} images each ({:.1f} memo hits, {:.1f} slow) | "
	    "a stage: key and lookup {:.2f} us, memo checks {:.2f} us, slow path {:.2f} us, BindImage "
	    "{:.2f} us\n",
	    d[7], d[4] / stages, d[5] / stages, d[6] / stages, d[0] / 1000.0 / stages,
	    d[1] / 1000.0 / stages, d[2] / 1000.0 / stages, d[3] / 1000.0 / stages));
}

void ReportCommit() {
	static std::array<uint64_t, 5> last {};
	static uint64_t                last_calls = 0;
	const auto                     calls      = g_commit_calls.load();
	const auto                     n          = static_cast<double>(std::max<uint64_t>(calls - last_calls, 1));
	last_calls                                = calls;
	double us[5];
	for (int i = 0; i < 5; i++) {
		const auto now = g_commit_ns[i].load();
		us[i]          = static_cast<double>(now - last[i]) / 1000.0 / n;
		last[i]        = now;
	}
	Log::WriteUnmuted(fmt::format(
	    "Native commit: {:.0f} marked draws' CommitBindings: {:.2f} us each = spec checks {:.2f}, "
	    "bindless {:.2f}, image transitions {:.2f}, descriptor writes {:.2f}, recording and the "
	    "rest {:.2f}\n",
	    n, us[0] + us[1] + us[2] + us[3] + us[4], us[0], us[1], us[2], us[3], us[4]));
}

void Report() {
	uint64_t last_t[11] = {};
	constexpr int Count = 13;
	uint64_t      last[Count] = {};
	uint64_t      last_presents = Common::PerfStats::PresentsTotal();
	for (;;) {
		std::this_thread::sleep_for(ReportEvery);
		const uint64_t now[Count] = {
		    g_markers_written.load(),    g_markers.load(),         g_orphans.load(),
		    g_draws.load(),              g_marked.load(),          g_block_match.load(),
		    g_no_block.load(),           g_with_mesh.load(),       g_binding_draws.load(),
		    g_binding_hits.load(),       g_binding_ns.load(),      g_binding_mismatches.load(),
		    g_binding_uncacheable.load()};
		const auto presents = Common::PerfStats::PresentsTotal();
		const auto frames   = static_cast<double>(std::max<uint64_t>(presents - last_presents, 1));
		double     d[Count];
		uint64_t   delta[Count];
		for (int i = 0; i < Count; i++) {
			delta[i] = now[i] - last[i];
			d[i]     = static_cast<double>(delta[i]) / frames;
			last[i]  = now[i];
		}
		last_presents = presents;
		Log::WriteUnmuted(fmt::format(
		    "Native draws: a frame: {:.0f} markers written, {:.0f} seen ({:.0f} without a draw) | "
		    "{:.0f} indexed draws, {:.0f} marked ({:.1f}%): {:.0f} with the pixel stage's texture "
		    "table at the material's block, {:.0f} whose material has no block, {:.0f} with a mesh "
		    "| {} materials so far | pixel bindings by material ({}): {:.0f} draws, {:.1f}% reused, "
		    "{:.2f} us each, {:.0f} uncacheable, {} verify mismatches\n",
		    d[0], d[1], d[2], d[3], d[4], d[3] == 0 ? 0.0 : 100.0 * d[4] / d[3], d[5], d[6], d[7],
		    g_material_count.load(), BindingsEnabled() ? "on" : "off", d[8],
		    delta[8] == 0 ? 0.0 : 100.0 * static_cast<double>(delta[9]) / static_cast<double>(delta[8]),
		    delta[8] == 0 ? 0.0 : static_cast<double>(delta[10]) / 1000.0 / static_cast<double>(delta[8]),
		    d[12], delta[11]));
		const uint64_t t[11] = {g_template_notes.load(),    g_template_previous.load(),
		                        g_template_same.load(),     g_template_older.load(),
		                        g_template_new.load(),      g_template_distinct.load(),
		                        g_marked_draw_ns.load(),    g_marked_draw_count.load(),
		                        g_other_draw_ns.load(),     g_other_draw_count.load(),
		                        0};
		double td[11];
		for (int i = 0; i < 11; i++) {
			td[i]     = static_cast<double>(t[i] - last_t[i]);
			last_t[i] = t[i];
		}
		const auto pct = [&](int i) { return td[0] == 0 ? 0.0 : 100.0 * td[i] / td[0]; };
		Log::WriteUnmuted(fmt::format(
		    "Native templates: a frame: {:.0f} marked draws keyed, {:.0f} distinct keys; of the draws "
		    "{:.1f}% keyed as the frame before, {:.1f}% earlier this frame, {:.1f}% older, {:.1f}% "
		    "new | renderer DrawIndex: marked {:.2f} us, others {:.2f} us\n",
		    td[0] / frames, td[5] / frames, pct(1), pct(2), pct(3), pct(4),
		    td[7] == 0 ? 0.0 : td[6] / 1000.0 / td[7], td[9] == 0 ? 0.0 : td[8] / 1000.0 / td[9]));
		ReportStateKeys();
		ReportKeyParts();
		ReportCommit();
		ReportResolve();
		ReportMutations();
	}
}

} // namespace

bool Enabled() {
	return g_enabled;
}

bool ForDraw(uint64_t guest_return_address, Identity* identity) {
	const auto offset = guest_return_address - g_main_base;
	const bool engine = (offset >= MeshDraw && offset < MeshDraw + MeshDrawSize) ||
	                    (offset >= DrawIndex && offset < DrawIndex + DrawIndexSize);
	if (!engine || t_identity.material == 0) {
		return false;
	}
	t_identity.sequence++;
	*identity = t_identity;
	g_markers_written.fetch_add(1, std::memory_order_relaxed);
	return true;
}

void WriteMarker(uint32_t* packet, const Identity& identity) {
	packet[0] = KYTY_PM4(MarkerDwords, Pm4::IT_NOP, Pm4::R_GT7_DRAW);
	const uint64_t fields[6] = {identity.set,     identity.material, identity.block,
	                            identity.shaders, identity.mesh,     identity.state};
	for (int i = 0; i < 6; i++) {
		packet[1 + i * 2] = static_cast<uint32_t>(fields[i]);
		packet[2 + i * 2] = static_cast<uint32_t>(fields[i] >> 32u);
	}
	packet[13] = identity.sequence;
}

void OnMarker(const uint32_t* payload, uint32_t dwords) {
	g_markers.fetch_add(1, std::memory_order_relaxed);
	if (dwords < MarkerDwords - 1) {
		return;
	}
	if (g_pending) {
		g_orphans.fetch_add(1, std::memory_order_relaxed);
	}
	const auto field = [&](int i) {
		return payload[i * 2] | (static_cast<uint64_t>(payload[i * 2 + 1]) << 32u);
	};
	g_pending_identity = {field(0), field(1), field(2), field(3), field(4), field(5), payload[12]};
	g_pending          = true;
}

void OnDraw(const uint32_t* ps_user_sgprs, uint32_t count) {
	g_draws.fetch_add(1, std::memory_order_relaxed);
	Mutations::Reset();
	if (!g_pending) {
		return;
	}
	g_pending = false;
	const auto& id = g_pending_identity;
	g_current_marked   = true;
	g_current_identity = id;
	g_marked.fetch_add(1, std::memory_order_relaxed);
	if (id.mesh != 0) {
		g_with_mesh.fetch_add(1, std::memory_order_relaxed);
	}
	if (g_materials.size() <= 1u << 20u && g_materials.insert(id.material).second) {
		g_material_count.fetch_add(1, std::memory_order_relaxed);
	}
	if (id.block == 0) {
		g_no_block.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if (count >= 4) {
		const uint64_t table = ps_user_sgprs[2] | (static_cast<uint64_t>(ps_user_sgprs[3]) << 32u);
		if (table == id.block) {
			g_block_match.fetch_add(1, std::memory_order_relaxed);
			g_current_material = id.material;
		}
	}
}

void OnDrawDone(uint64_t renderer_ns) {
	if (g_current_marked) {
		const auto masks = Mutations::t_masks;
		g_mutation_draws.fetch_add(1, std::memory_order_relaxed);
		if (masks[0] == 0) {
			g_prepare_pure.fetch_add(1, std::memory_order_relaxed);
		}
		if (masks[1] == 0) {
			g_record_pure.fetch_add(1, std::memory_order_relaxed);
		}
		if ((masks[0] | masks[1]) == 0) {
			g_pure_draws.fetch_add(1, std::memory_order_relaxed);
			g_pure_ns.fetch_add(renderer_ns, std::memory_order_relaxed);
		} else {
			g_impure_ns.fetch_add(renderer_ns, std::memory_order_relaxed);
		}
		for (uint32_t phase = 0; phase < 2; phase++) {
			for (uint32_t i = 0; i < Mutations::Count; i++) {
				if ((masks[phase] >> i) & 1u) {
					g_mutation_counts[phase][i].fetch_add(1, std::memory_order_relaxed);
				}
			}
		}
		g_marked_draw_ns.fetch_add(renderer_ns, std::memory_order_relaxed);
		g_marked_draw_count.fetch_add(1, std::memory_order_relaxed);
	} else {
		g_other_draw_ns.fetch_add(renderer_ns, std::memory_order_relaxed);
		g_other_draw_count.fetch_add(1, std::memory_order_relaxed);
	}
	g_current_material = 0;
	g_current_marked   = false;
}

bool CurrentMarked() {
	return g_current_marked;
}

void AddResolveSteps(uint64_t key_ns, uint64_t memo_ns, uint64_t slow_ns, uint64_t bind_ns,
                     uint32_t images, uint32_t hits, uint32_t slows) {
	const uint64_t values[8] = {key_ns, memo_ns, slow_ns, bind_ns, images, hits, slows, 1};
	for (int i = 0; i < 8; i++) {
		g_resolve[i].fetch_add(values[i], std::memory_order_relaxed);
	}
}

void AddCommitSteps(const uint64_t* nanoseconds) {
	for (int i = 0; i < 5; i++) {
		g_commit_ns[i].fetch_add(nanoseconds[i], std::memory_order_relaxed);
	}
	g_commit_calls.fetch_add(1, std::memory_order_relaxed);
}

void NoteTemplate(uint64_t programs, uint64_t pipeline) {
	if (!g_current_marked) {
		return;
	}
	const auto& id    = g_current_identity;
	NoteStateChoice(id.state, id.material, programs, pipeline);
	uint64_t    key   = id.material;
	for (const uint64_t part: {id.mesh, programs, pipeline}) {
		key = (key ^ part) * 0x9E3779B97F4A7C15ull + (key >> 29u);
	}
	const auto frame = Common::PerfStats::PresentsTotal();
	if (frame != g_template_frame) {
		g_template_distinct.fetch_add(g_template_frame_keys.size(), std::memory_order_relaxed);
		g_template_frame_keys.clear();
		g_template_frame = frame;
	}
	g_template_frame_keys.insert(key);
	g_template_notes.fetch_add(1, std::memory_order_relaxed);
	if (g_template_frames.size() > (1u << 21u)) {
		g_template_frames.clear();
	}
	auto [it, inserted] = g_template_frames.try_emplace(key, frame);
	if (inserted) {
		g_template_new.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if (it->second == frame) {
		g_template_same.fetch_add(1, std::memory_order_relaxed);
	} else if (it->second + 1 == frame) {
		g_template_previous.fetch_add(1, std::memory_order_relaxed);
	} else {
		g_template_older.fetch_add(1, std::memory_order_relaxed);
	}
	it->second = frame;
}

uint64_t CurrentPixelMaterial() {
	return g_current_material;
}

bool BindingsEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GT7_NATIVE_BINDINGS");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

bool BindingsVerify() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GT7_NATIVE_VERIFY");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

void AddPixelBindings(bool hit, bool uncacheable, uint64_t nanoseconds) {
	g_binding_draws.fetch_add(1, std::memory_order_relaxed);
	g_binding_ns.fetch_add(nanoseconds, std::memory_order_relaxed);
	if (hit) {
		g_binding_hits.fetch_add(1, std::memory_order_relaxed);
	}
	if (uncacheable) {
		g_binding_uncacheable.fetch_add(1, std::memory_order_relaxed);
	}
}

void AddBindingMismatch() {
	g_binding_mismatches.fetch_add(1, std::memory_order_relaxed);
}

void Install(uint64_t main_base) {
	const char* enabled = std::getenv("KYTY_GT7_NATIVE");
	if (enabled == nullptr || enabled[0] != '1') {
		return;
	}
	auto* bind = reinterpret_cast<uint8_t*>(main_base + BindMaterial);
	auto* mesh = reinterpret_cast<uint8_t*>(main_base + MeshDraw);
	if (std::memcmp(bind, BindMoved.data(), BindMoved.size()) != 0) {
		Log::WriteToConsoleAndLog("Native draws: not installed (gt7_BindMaterial differs from GT7 "
		                          "v01.710's)\n");
		return;
	}
	const auto page = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    main_base, 0x4000, Common::VirtualMemory::Mode::ExecuteReadWrite, "gt7_native_draws", false);
	if (page == 0) {
		Log::WriteToConsoleAndLog("Native draws: not installed (no memory for the stubs)\n");
		return;
	}
	Gt7GuestHook::Emitter b {reinterpret_cast<uint8_t*>(page)};
	const auto bind_stub = b.Here();
	b.CallHost(reinterpret_cast<const void*>(&OnBindMaterial)); // rdi, rsi, rdx, rcx
	b.Return(BindMoved, main_base + BindMaterial);
	// KYTY_DRAW_COUNTS hooks the same entry: then the mesh is left out
	const bool mesh_free = std::memcmp(mesh, MeshMoved.data(), MeshMoved.size()) == 0;
	// N1: the commit's state key (KYTY_TEXTURE_COUNTS hooks the same entry: then none)
	auto*      commit      = reinterpret_cast<uint8_t*>(main_base + CommitDrawState);
	const bool commit_free = std::memcmp(commit, CommitMoved.data(), CommitMoved.size()) == 0;
	uint64_t   commit_stub = 0;
	if (commit_free) {
		Gt7GuestHook::Emitter c {reinterpret_cast<uint8_t*>(page + 0x400)};
		commit_stub = c.Here();
		c.CallHost(reinterpret_cast<const void*>(&OnCommitState)); // rdi: the render context
		c.Return(CommitMoved, main_base + CommitDrawState);
	}
	uint64_t   mesh_stub = 0;
	if (mesh_free) {
		Gt7GuestHook::Emitter m {reinterpret_cast<uint8_t*>(page + 0x200)};
		mesh_stub = m.Here();
		m.CallHost(reinterpret_cast<const void*>(&OnMeshDraw)); // rdi: the mesh
		m.Return(MeshMoved, main_base + MeshDraw);
	}
	Common::VirtualMemory::FlushInstructionCache(page, 0x600);
	Gt7GuestHook::PatchEntry(bind, BindMoved.size(), bind_stub);
	if (commit_free) {
		Gt7GuestHook::PatchEntry(commit, CommitMoved.size(), commit_stub);
	}
	if (mesh_free) {
		Gt7GuestHook::PatchEntry(mesh, MeshMoved.size(), mesh_stub);
	}
	g_main_base = main_base;
	g_enabled   = true;
	std::thread(Report).detach();
	Log::WriteToConsoleAndLog(fmt::format("Native draws: installed on gt7_BindMaterial{}{}\n",
	                                      mesh_free ? ", gt7_DrawMeshClusters" : "",
	                                      commit_free ? ", gt7_CommitDrawState" : ""));
}

} // namespace Loader::Gt7NativeDraws
