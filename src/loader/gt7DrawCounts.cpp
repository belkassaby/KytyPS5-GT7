#include "loader/gt7DrawCounts.h"

#include "common/logging/log.h"
#include "common/perfStats.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "loader/gt7GuestHook.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <tuple>

#include <fmt/format.h>

namespace Loader::Gt7DrawCounts {

namespace {

constexpr uint64_t MeshDraw      = 0x5fb0;  // gt7_DrawMeshClusters, from the eboot base
constexpr uint64_t MeshDrawSize  = 9124;
constexpr uint64_t DrawIndex     = 0xa1400; // gt7_DrawIndex
constexpr auto     ReportEvery   = std::chrono::seconds(10);

// The prologues the stubs run instead (no RIP-relative operands, so they run anywhere):
// push rbp; mov rbp, rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp, ...
constexpr std::array<uint8_t, 20> MeshPrologue = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                                  0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                                  0x81, 0xec, 0xe8, 0x06, 0x00, 0x00};
constexpr std::array<uint8_t, 17> DrawPrologue = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57,
                                                  0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                                  0x53, 0x48, 0x83, 0xec, 0x28};

// In the stub page, read and written by the stubs with lock-prefixed instructions.
struct Counters {
	uint64_t mesh_calls;
	uint64_t clusters;
	uint64_t draws_from_mesh;
	uint64_t draws;
	uint64_t mesh_begin;
};

using Gt7GuestHook::Emitter;

Counters* g_counters = nullptr;

uint64_t Load(const uint64_t& v) {
	return __atomic_load_n(&v, __ATOMIC_RELAXED);
}

void Report() {
	Counters last {};
	uint64_t last_presents = Common::PerfStats::PresentsTotal();
	auto     last_time     = std::chrono::steady_clock::now();
	for (;;) {
		std::this_thread::sleep_for(ReportEvery);
		const Counters now {Load(g_counters->mesh_calls), Load(g_counters->clusters),
		                    Load(g_counters->draws_from_mesh), Load(g_counters->draws), 0};
		const auto presents = Common::PerfStats::PresentsTotal();
		const auto time     = std::chrono::steady_clock::now();
		const auto frames   = static_cast<double>(std::max<uint64_t>(presents - last_presents, 1));
		const auto meshes   = static_cast<double>(now.mesh_calls - last.mesh_calls);
		const auto clusters = static_cast<double>(now.clusters - last.clusters);
		const auto runs     = static_cast<double>(now.draws_from_mesh - last.draws_from_mesh);
		const auto draws    = static_cast<double>(now.draws - last.draws);
		Log::WriteUnmuted(fmt::format(
		    "Draw counts: {:.1f} s, {:.0f} frames | a frame: {:.0f} mesh draws (gt7_DrawMeshClusters) "
		    "given {:.0f} clusters, {:.0f} DrawIndex from them ({:.1f} a mesh draw), {:.0f} "
		    "DrawIndex in all\n",
		    std::chrono::duration<double>(time - last_time).count(), frames, meshes / frames,
		    clusters / frames, runs / frames, meshes == 0 ? 0.0 : runs / meshes, draws / frames));
		last          = now;
		last_presents = presents;
		last_time     = time;
	}
}

} // namespace

void Install(uint64_t main_base) {
	const char* enabled = std::getenv("KYTY_DRAW_COUNTS");
	if (enabled == nullptr || enabled[0] != '1') {
		return;
	}
	auto* mesh = reinterpret_cast<uint8_t*>(main_base + MeshDraw);
	auto* draw = reinterpret_cast<uint8_t*>(main_base + DrawIndex);
	if (std::memcmp(mesh, MeshPrologue.data(), MeshPrologue.size()) != 0 ||
	    std::memcmp(draw, DrawPrologue.data(), DrawPrologue.size()) != 0) {
		Log::WriteToConsoleAndLog("Draw counts: not installed (the draw functions differ from GT7 "
		                          "v01.710's)\n");
		return;
	}
	// guest runtime memory, as the game patches' code caves (a plain host allocation is refused)
	const auto page = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    main_base, 0x4000, Common::VirtualMemory::Mode::ExecuteReadWrite, "gt7_draw_counts", false);
	if (page == 0) {
		Log::WriteToConsoleAndLog("Draw counts: not installed (no memory for the stubs)\n");
		return;
	}
	g_counters             = reinterpret_cast<Counters*>(page);
	*g_counters            = {};
	g_counters->mesh_begin = main_base + MeshDraw;

	// gt7_DrawMeshClusters: count the call and its cluster_count (argument 11, at [rsp+0x28])
	Emitter m {reinterpret_cast<uint8_t*>(page + 0x100)};
	const auto mesh_stub = m.Here();
	m.RipRelative({0xf0, 0x48, 0xff, 0x05}, &g_counters->mesh_calls); // lock inc qword [rip]
	m.Bytes({0x50});                                                   // push rax
	m.Bytes({0x8b, 0x44, 0x24, 0x30});                                 // mov eax, [rsp+0x30]
	m.RipRelative({0xf0, 0x48, 0x01, 0x05}, &g_counters->clusters);    // lock add [rip], rax
	m.Bytes({0x58});                                                   // pop rax
	m.Return(MeshPrologue, main_base + MeshDraw);

	// gt7_DrawIndex: count the call, and apart the ones returning into gt7_DrawMeshClusters
	Emitter d {reinterpret_cast<uint8_t*>(page + 0x200)};
	const auto draw_stub = d.Here();
	d.RipRelative({0xf0, 0x48, 0xff, 0x05}, &g_counters->draws);      // lock inc qword [rip]
	d.Bytes({0x50});                                                   // push rax
	d.Bytes({0x48, 0x8b, 0x44, 0x24, 0x08});                           // mov rax, [rsp+8]
	d.RipRelative({0x48, 0x2b, 0x05}, &g_counters->mesh_begin);        // sub rax, [rip]
	d.Bytes({0x48, 0x3d});                                             // cmp rax, imm32
	d.U32(static_cast<uint32_t>(MeshDrawSize));
	d.Bytes({0x73, 0x08});                                             // jae over the inc
	d.RipRelative({0xf0, 0x48, 0xff, 0x05}, &g_counters->draws_from_mesh);
	d.Bytes({0x58});                                                   // pop rax
	d.Return(DrawPrologue, main_base + DrawIndex);
	Common::VirtualMemory::FlushInstructionCache(page, 0x1000);

	// the entries jump to the stubs; the rest of each moved prologue is never reached
	Gt7GuestHook::PatchEntry(mesh, MeshPrologue.size(), mesh_stub);
	Gt7GuestHook::PatchEntry(draw, DrawPrologue.size(), draw_stub);
	std::thread(Report).detach();
	Log::WriteToConsoleAndLog("Draw counts: installed on gt7_DrawMeshClusters and gt7_DrawIndex\n");
}

} // namespace Loader::Gt7DrawCounts
