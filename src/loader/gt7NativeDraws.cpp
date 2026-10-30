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
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

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
constexpr auto     ReportEvery      = std::chrono::seconds(10);

// push rbp; mov rbp, rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp, 0x138
constexpr std::array<uint8_t, 20> BindMoved = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                               0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                               0x81, 0xec, 0x38, 0x01, 0x00, 0x00};
// the same with sub rsp, 0x6e8
constexpr std::array<uint8_t, 20> MeshMoved = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                               0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                               0x81, 0xec, 0xe8, 0x06, 0x00, 0x00};

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

// Milestone 4, measured first: what marked draws changed in the renderer's shared state
// (Libs::Graphics::DrawMutations), by kind, and how many changed nothing
namespace Mutations = Libs::Graphics::DrawMutations;
std::array<std::atomic<uint64_t>, Mutations::Count> g_mutation_counts {};
std::atomic<uint64_t>                               g_mutation_draws {0};
std::atomic<uint64_t>                               g_pure_draws {0};
std::atomic<uint64_t>                               g_pure_ns {0};
std::atomic<uint64_t>                               g_impure_ns {0};

void ReportMutations() {
	static std::array<uint64_t, Mutations::Count> last {};
	static uint64_t last_draws = 0, last_pure = 0, last_pure_ns = 0, last_impure_ns = 0;
	const auto draws = g_mutation_draws.load(), pure = g_pure_draws.load();
	const auto pure_ns = g_pure_ns.load(), impure_ns = g_impure_ns.load();
	const auto n       = static_cast<double>(std::max<uint64_t>(draws - last_draws, 1));
	const auto p       = static_cast<double>(pure - last_pure);
	const char* names[Mutations::Count] = {"barrier", "render pass", "new image", "image upload",
	                                       "new buffer", "buffer upload", "new pipeline"};
	std::string kinds;
	for (uint32_t i = 0; i < Mutations::Count; i++) {
		const auto now = g_mutation_counts[i].load();
		kinds += fmt::format("{}{} {:.1f}%", i == 0 ? "" : ", ", names[i],
		                     100.0 * static_cast<double>(now - last[i]) / n);
		last[i] = now;
	}
	Log::WriteUnmuted(fmt::format(
	    "Native mutations: {:.0f} marked draws: {:.1f}% changed no shared state ({:.2f} us each in "
	    "the renderer; the others {:.2f} us) | draws with each kind: {}\n",
	    n, 100.0 * p / n, p == 0 ? 0.0 : static_cast<double>(pure_ns - last_pure_ns) / 1000.0 / p,
	    n - p == 0 ? 0.0 : static_cast<double>(impure_ns - last_impure_ns) / 1000.0 / (n - p), kinds));
	last_draws     = draws;
	last_pure      = pure;
	last_pure_ns   = pure_ns;
	last_impure_ns = impure_ns;
}

// CommitBindings' steps for marked draws (AddCommitSteps)
std::array<std::atomic<uint64_t>, 5> g_commit_ns {};
std::atomic<uint64_t>                g_commit_calls {0};

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
		ReportCommit();
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
	const uint64_t fields[5] = {identity.set, identity.material, identity.block, identity.shaders,
	                            identity.mesh};
	for (int i = 0; i < 5; i++) {
		packet[1 + i * 2] = static_cast<uint32_t>(fields[i]);
		packet[2 + i * 2] = static_cast<uint32_t>(fields[i] >> 32u);
	}
	packet[11] = identity.sequence;
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
	g_pending_identity = {field(0), field(1), field(2), field(3), field(4), payload[10]};
	g_pending          = true;
}

void OnDraw(const uint32_t* ps_user_sgprs, uint32_t count) {
	g_draws.fetch_add(1, std::memory_order_relaxed);
	Mutations::t_mask = 0;
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
		const auto mask = Mutations::t_mask;
		g_mutation_draws.fetch_add(1, std::memory_order_relaxed);
		if (mask == 0) {
			g_pure_draws.fetch_add(1, std::memory_order_relaxed);
			g_pure_ns.fetch_add(renderer_ns, std::memory_order_relaxed);
		} else {
			g_impure_ns.fetch_add(renderer_ns, std::memory_order_relaxed);
		}
		for (uint32_t i = 0; i < Mutations::Count; i++) {
			if ((mask >> i) & 1u) {
				g_mutation_counts[i].fetch_add(1, std::memory_order_relaxed);
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
	uint64_t   mesh_stub = 0;
	if (mesh_free) {
		Gt7GuestHook::Emitter m {reinterpret_cast<uint8_t*>(page + 0x200)};
		mesh_stub = m.Here();
		m.CallHost(reinterpret_cast<const void*>(&OnMeshDraw)); // rdi: the mesh
		m.Return(MeshMoved, main_base + MeshDraw);
	}
	Common::VirtualMemory::FlushInstructionCache(page, 0x400);
	Gt7GuestHook::PatchEntry(bind, BindMoved.size(), bind_stub);
	if (mesh_free) {
		Gt7GuestHook::PatchEntry(mesh, MeshMoved.size(), mesh_stub);
	}
	g_main_base = main_base;
	g_enabled   = true;
	std::thread(Report).detach();
	Log::WriteToConsoleAndLog(fmt::format("Native draws: installed on gt7_BindMaterial{}\n",
	                                      mesh_free ? " and gt7_DrawMeshClusters" : ""));
}

} // namespace Loader::Gt7NativeDraws
