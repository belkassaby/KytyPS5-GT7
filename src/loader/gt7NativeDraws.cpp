#include "loader/gt7NativeDraws.h"

#include "common/abi.h"
#include "common/logging/log.h"
#include "common/perfStats.h"
#include "common/virtualMemory.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"
#include "graphics/host_gpu/renderer/drawMutations.h"
#include "loader/gt7GuestHook.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

namespace Loader::Gt7NativeDraws {

// N3 step 3 (defined with the public functions below, used by the report and the draws).
void ReportUse();
void ReportImages();
void ReportBuffers();
void ReportDcbBuffers();
void FreeRecordStages();

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

// N3 (KYTY_GT7_NATIVE_RECORDS=1): records made at the markers and checked at the draws; with
// =walk, also each stage's descriptor walk on the render thread (step 2).
bool RecordsEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GT7_NATIVE_RECORDS");
		return value != nullptr && (value[0] == '1' || std::strcmp(value, "walk") == 0 ||
		                            std::strcmp(value, "use") == 0);
	}();
	return enabled;
}

bool RecordsUseEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GT7_NATIVE_RECORDS");
		return value != nullptr && std::strcmp(value, "use") == 0;
	}();
	return enabled;
}

// =walk, and =use (step 3) which walks too
bool RecordsWalkEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GT7_NATIVE_RECORDS");
		return value != nullptr && (std::strcmp(value, "walk") == 0 || std::strcmp(value, "use") == 0);
	}();
	return enabled;
}

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

// The render thread's context at its last commit (the draw that follows uses it).
thread_local const uint8_t* t_ctx = nullptr;

// N3 (docs/GT7-NATIVE-BACKEND.md): the draw's record, made on the render thread at the marker
// (after the commit, which may renew the constants): each of the vertex (0) and pixel (1)
// stages' constant block, copied into the thread's ring. The GPU thread checks at the draw that
// the stage's constant buffer descriptor (its 4 SGPRs at constants_slot) points at the block, and
// that guest memory still holds what the render thread copied. A record whose ring space was
// reused before the GPU thread came to it is counted, not read.
constexpr uint64_t StageConstants     = 0x10;
constexpr uint64_t StageConstantsSlot = 0xa0;
constexpr uint64_t StageConstantsSize = 0xa2;
constexpr size_t   RingBytes          = size_t {64} << 20u;
constexpr uint32_t MaxConstants       = 0x4000;

struct RecordStage {
	uint64_t guest = 0;
	uint32_t size  = 0;
	uint32_t slot  = 0;
	uint32_t offset = 0; // of the copy, from the record's start
};
// N3 step 1: each stage's user-data SGPRs 0-31 from the engine's shadow (GT7StageBinding +0x18),
// compared at the draw with the SGPRs Kyty's command processor holds.
constexpr uint64_t StageUserData    = 0x18;
constexpr uint64_t ShaderTypeOffset = 0x5a; // in the AGC shader header
struct Record {
	uint64_t    serial = 0;
	RecordStage stages[2];
	uint32_t    user[2][32] {};
	bool        has_user[2] {};
	uint8_t     user_type[2] {};  // the shader's type (ShaderBinaryType)
	uint8_t     user_stage[2] {}; // the GT7 stage it came from
	// Step 2: the walk of each stage on the render thread (0 not walked, 1 walked, 2 unreadable).
	uint8_t     walk_state[2] {};
	uint64_t    walk_hash[2] {};
	// Step 3: the stages prepared for the GPU thread to take (owned by the record until the GPU
	// thread's draw ends; it frees them).
	Libs::Graphics::DrawPrep::StageResult* prepared[2] {};
	// N4: the pixel stage's images as the render thread found them (Gt7NativeWalk::ResolveImages).
	uint32_t image_ids[32] {};
	uint8_t  image_states[32] {};
	uint8_t  image_count = 0;
	// N4 step 1: the pixel images' whole bindings (heap, freed by the GPU thread after the draw).
	std::array<std::shared_ptr<const void>, 32>* image_bindings = nullptr;
	// The vertex and pixel stages' buffers (Gt7NativeWalk::ResolveBuffers).
	uint32_t buffer_ids[2][32] {};
	uint8_t  buffer_states[2][32] {};
	uint8_t  buffer_count[2] {};
};
struct RecordRing {
	std::unique_ptr<uint8_t[]> bytes {new uint8_t[RingBytes]};
	size_t                     head   = 0;
	uint64_t                   serial = 0;
};
thread_local RecordRing t_ring;
std::atomic<uint64_t>   g_record_serials {0}; // serials unique across threads
std::atomic<uint64_t>   g_records_made {0}, g_records_ns {0}, g_record_bytes {0};

// Step 2: the programs the GPU thread learned, by state key, material and stage.
std::shared_mutex                                                         g_learned_mutex;
std::unordered_map<uint64_t, Libs::Graphics::Gt7NativeWalk::Learned>      g_learned;
std::atomic<uint64_t> g_walks {0}, g_walk_ns {0}, g_walk_unreadable {0}, g_walk_unknown {0};
std::array<std::atomic<uint64_t>, 4> g_walk_why {}; // by Gt7NativeWalk::LastFailure

uint64_t LearnedKey(uint64_t state, uint64_t material, uint32_t stage) {
	return ((state ^ (material * 0x9E3779B97F4A7C15ull)) << 1u) | stage;
}

void ClassifyBuffers(uint32_t stage, uint32_t count, const uint8_t* states,
                     const uint64_t* addresses, const uint64_t* sizes);

void WalkRecord(Record& record) {
	for (uint32_t s = 0; s < 2; s++) {
		if (!record.has_user[s]) {
			continue;
		}
		Libs::Graphics::Gt7NativeWalk::Learned learned;
		{
			std::shared_lock lock(g_learned_mutex);
			const auto it = g_learned.find(LearnedKey(t_identity.state, t_identity.material, s));
			if (it == g_learned.end()) {
				g_walk_unknown.fetch_add(1, std::memory_order_relaxed);
				continue;
			}
			learned = it->second;
		}
		const auto start = std::chrono::steady_clock::now();
		uint64_t   hash  = 0;
		if (RecordsUseEnabled()) {
			auto* prepared = new Libs::Graphics::DrawPrep::StageResult;
			if (Libs::Graphics::Gt7NativeWalk::Prepare(
			        learned, record.user[s],
			        learned.stage,
			        prepared)) {
				record.prepared[s]   = prepared;
				record.walk_state[s] = 1;
				if (RecordsCheck()) {
					std::array<uint64_t, 32> addresses {};
					std::array<uint64_t, 32> sizes {};
					record.walk_hash[s]    = Libs::Graphics::Gt7NativeWalk::Hash(prepared->resources);
					record.buffer_count[s] = static_cast<uint8_t>(Libs::Graphics::Gt7NativeWalk::ResolveBuffers(
					    LearnedKey(t_identity.state, t_identity.material, s), prepared->resources,
					    record.buffer_ids[s], record.buffer_states[s], 32, addresses.data(),
					    sizes.data()));
					ClassifyBuffers(s, record.buffer_count[s], record.buffer_states[s],
					                addresses.data(), sizes.data());
				}
				if (s == 1) {
					record.image_bindings = new std::array<std::shared_ptr<const void>, 32>;
					record.image_count = static_cast<uint8_t>(Libs::Graphics::Gt7NativeWalk::ResolveImages(
					    LearnedKey(t_identity.state, t_identity.material, 1), prepared->resources,
					    record.image_ids, record.image_states, 32, record.image_bindings->data()));
				}
			} else {
				delete prepared;
				record.walk_state[s] = 2;
				g_walk_unreadable.fetch_add(1, std::memory_order_relaxed);
			}
		} else if (Libs::Graphics::Gt7NativeWalk::Walk(learned, record.user[s], &hash)) {
			record.walk_state[s] = 1;
			record.walk_hash[s]  = hash;
		} else {
			record.walk_state[s] = 2;
			g_walk_unreadable.fetch_add(1, std::memory_order_relaxed);
			uint64_t       address = 0;
			uint32_t       size    = 0;
			const uint32_t why     = Libs::Graphics::Gt7NativeWalk::LastFailure(&address, &size);
			g_walk_why[std::min<uint32_t>(why, 3)].fetch_add(1, std::memory_order_relaxed);
			static std::atomic<uint32_t> logged {0};
			if (why == 2 && logged.fetch_add(1, std::memory_order_relaxed) < 24) {
				Log::WriteUnmuted(fmt::format(
				    "Native records N3 walks: {} stage read refused at 0x{:x} ({} bytes), user data "
				    "{}: {:08x} {:08x} {:08x} {:08x}\n",
				    s == 0 ? "vertex" : "pixel", address, size, learned.user_data_count,
				    record.user[s][0], record.user[s][1], record.user[s][2], record.user[s][3]));
			}
		}
		g_walks.fetch_add(1, std::memory_order_relaxed);
		g_walk_ns.fetch_add(static_cast<uint64_t>(
		                        std::chrono::duration_cast<std::chrono::nanoseconds>(
		                            std::chrono::steady_clock::now() - start)
		                            .count()),
		                    std::memory_order_relaxed);
	}
}

// N4 step 3, sub-step 1, measured first: a record's buffers in the render thread's own command
// buffer segment (GT7CommandBuffer at ctx+0x788: [bottom, top); data is allocated down from top):
// memory it wrote and the GPU never writes, which a record can copy into a ring of its own.
constexpr uint64_t CtxDcb       = 0x788;
std::array<std::atomic<uint64_t>, 2> g_dcb_buffers {}, g_dcb_bytes {}, g_other_buffers {},
    g_other_bytes {}, g_dcb_stages_all {}, g_dcb_stages {};

void ClassifyBuffers(uint32_t stage, uint32_t count, const uint8_t* states,
                     const uint64_t* addresses, const uint64_t* sizes) {
	if (t_ctx == nullptr || count == 0) {
		return;
	}
	// The render context's command buffer, and each stage's (GT7StageBinding +0xa8: where
	// gt7_RenewStageConstants moves the stage's constants), as [bottom, top).
	std::array<std::pair<uint64_t, uint64_t>, 7> segments {};
	std::memcpy(&segments[0].first, t_ctx + CtxDcb, sizeof(uint64_t));
	std::memcpy(&segments[0].second, t_ctx + CtxDcb + 8, sizeof(uint64_t));
	for (uint64_t st = 0; st < 6; st++) {
		uint64_t dcb = 0;
		std::memcpy(&dcb, t_ctx + CtxStages + st * CtxStageSize + 0xa8, sizeof(dcb));
		if (dcb != 0) {
			std::memcpy(&segments[1 + st].first, reinterpret_cast<const void*>(dcb), sizeof(uint64_t));
			std::memcpy(&segments[1 + st].second, reinterpret_cast<const void*>(dcb + 8), sizeof(uint64_t));
		}
	}
	static std::atomic<uint32_t> logged {0};
	bool all = true;
	for (uint32_t i = 0; i < count; i++) {
		if (states[i] == 0) {
			continue; // none bound
		}
		bool in = false;
		for (const auto& [bottom, top]: segments) {
			in |= bottom != 0 && addresses[i] >= bottom && addresses[i] + sizes[i] <= top;
		}
		if (logged.fetch_add(1, std::memory_order_relaxed) < 12) {
			Log::WriteUnmuted(fmt::format(
			    "Native records N4 command buffer data: {} buffer at 0x{:x} ({} bytes): {} | ctx "
			    "[0x{:x}, 0x{:x}) stage 0 [0x{:x}, 0x{:x}) stage 1 [0x{:x}, 0x{:x})\n",
			    stage == 0 ? "vertex" : "pixel", addresses[i], sizes[i], in ? "in" : "outside",
			    segments[0].first, segments[0].second, segments[1].first, segments[1].second,
			    segments[2].first, segments[2].second));
		}
		(in ? g_dcb_buffers : g_other_buffers)[stage].fetch_add(1, std::memory_order_relaxed);
		(in ? g_dcb_bytes : g_other_bytes)[stage].fetch_add(sizes[i], std::memory_order_relaxed);
		all &= in;
	}
	g_dcb_stages[stage].fetch_add(1, std::memory_order_relaxed);
	if (all) {
		g_dcb_stages_all[stage].fetch_add(1, std::memory_order_relaxed);
	}
}

void ReportDcbBuffers() {
	static std::array<std::array<uint64_t, 6>, 2> last {};
	std::string                                   text;
	for (uint32_t s = 0; s < 2; s++) {
		const std::array<uint64_t, 6> now = {g_dcb_stages[s].load(),  g_dcb_stages_all[s].load(),
		                                     g_dcb_buffers[s].load(), g_dcb_bytes[s].load(),
		                                     g_other_buffers[s].load(), g_other_bytes[s].load()};
		std::array<double, 6>          d {};
		for (size_t i = 0; i < d.size(); i++) {
			d[i]       = static_cast<double>(now[i] - last[s][i]);
			last[s][i] = now[i];
		}
		const double stages = std::max(1.0, d[0]);
		text += fmt::format(
		    " | {} stages {:.0f}: every buffer in the command buffer {:.1f}%; buffers in it {:.0f} "
		    "({:.0f} bytes each), elsewhere {:.0f} ({:.0f} bytes each)",
		    s == 0 ? "vertex" : "pixel", d[0], 100.0 * d[1] / stages, d[2],
		    d[2] == 0 ? 0.0 : d[3] / d[2], d[4], d[4] == 0 ? 0.0 : d[5] / d[4]);
	}
	Log::WriteUnmuted(fmt::format("Native records N4 command buffer data (10 s){}\n", text));
}

uint64_t MakeRecord() {
	if (t_ctx == nullptr) {
		return 0;
	}
	const auto start = std::chrono::steady_clock::now();
	auto&      ring  = t_ring;
	size_t     need  = sizeof(Record);
	uint32_t   sizes[2] {};
	uint64_t   blocks[2] {};
	uint32_t   slots[2] {};
	for (uint32_t s = 0; s < 2; s++) {
		const uint8_t* stage = t_ctx + CtxStages + s * CtxStageSize;
		uint64_t       shader = 0;
		std::memcpy(&shader, stage, sizeof(shader));
		std::memcpy(&blocks[s], stage + StageConstants, sizeof(uint64_t));
		uint16_t size = 0;
		uint16_t slot = 0;
		std::memcpy(&size, stage + StageConstantsSize, sizeof(size));
		std::memcpy(&slot, stage + StageConstantsSlot, sizeof(slot));
		if (shader == 0 || blocks[s] == 0 || size == 0 || size > MaxConstants) {
			continue;
		}
		sizes[s] = size;
		slots[s] = slot;
		need += (size + 15u) & ~15u;
	}
	if (ring.head + need > RingBytes) {
		ring.head = 0;
	}
	auto*   base   = ring.bytes.get() + ring.head;
	auto*   record = new (base) Record {};
	record->serial = g_record_serials.fetch_add(1, std::memory_order_relaxed) + 1;
	// The hardware stage is the shader's type (its header's byte at +0x5a), not the GT7 stage's
	// index: the pixel stage is the one whose shader is a PS (1), the vertex stage the first GS
	// (2, 4 or 6). A GS-back's (6) user data goes through memory (gt7_WriteStageUserData), not
	// SGPRs: counted apart.
	for (uint32_t s = 0; s < 6; s++) {
		const uint8_t* stage  = t_ctx + CtxStages + s * CtxStageSize;
		uint64_t       shader = 0;
		std::memcpy(&shader, stage, sizeof(shader));
		if (shader == 0) {
			continue;
		}
		const uint8_t type = *reinterpret_cast<const uint8_t*>(shader + ShaderTypeOffset);
		const int     hw   = type == 1                           ? 1
		                     : (type == 2 || type == 4 || type == 6) ? 0
		                                                         : -1;
		if (hw < 0 || record->has_user[hw]) {
			continue;
		}
		std::memcpy(record->user[hw], stage + StageUserData, sizeof(record->user[hw]));
		record->has_user[hw]   = true;
		record->user_type[hw]  = type;
		record->user_stage[hw] = static_cast<uint8_t>(s);
	}
	if (RecordsWalkEnabled()) {
		WalkRecord(*record);
	}
	uint32_t offset = sizeof(Record);
	for (uint32_t s = 0; s < 2; s++) {
		if (sizes[s] == 0) {
			continue;
		}
		std::memcpy(base + offset, reinterpret_cast<const void*>(blocks[s]), sizes[s]);
		record->stages[s] = {blocks[s], sizes[s], slots[s], offset};
		offset += (sizes[s] + 15u) & ~15u;
		g_record_bytes.fetch_add(sizes[s], std::memory_order_relaxed);
	}
	ring.head += need;
	g_records_made.fetch_add(1, std::memory_order_relaxed);
	g_records_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                                 std::chrono::steady_clock::now() - start)
	                                                 .count()),
	                       std::memory_order_relaxed);
	// The record's address and serial: the GPU thread checks the serial is still the record's.
	return reinterpret_cast<uint64_t>(record);
}

// GPU thread, at the draw: the checks of a marked draw's record.
std::atomic<uint64_t> g_n3_draws {0}, g_n3_stale {0}, g_n3_stages {0}, g_n3_address {0},
    g_n3_bytes {0}, g_n3_no_slot {0};
// Step 1: stages compared, stages whose SGPRs all matched, SGPRs compared, and per SGPR index the
// mismatches (vertex stage, pixel stage).
std::atomic<uint64_t> g_user_stages {0}, g_user_stages_same {0}, g_user_sgprs {0};
// by the shader's type (0..8) and GT7 stage (0..5): stages compared, all SGPRs the same
std::array<std::array<std::atomic<uint64_t>, 6>, 9> g_user_by_type {}, g_user_same_by_type {};
std::array<std::array<std::atomic<uint64_t>, 32>, 2> g_user_diff {};

void CheckRecord(uint64_t record_address, uint64_t serial, const uint32_t* const sgprs[2],
                 const uint32_t counts[2]) {
	if (record_address == 0) {
		return;
	}
	g_n3_draws.fetch_add(1, std::memory_order_relaxed);
	const auto* base   = reinterpret_cast<const uint8_t*>(record_address);
	const auto* record = reinterpret_cast<const Record*>(base);
	if (record->serial != serial) {
		g_n3_stale.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	for (uint32_t s = 0; s < 2; s++) {
		if (!record->has_user[s] || sgprs[s] == nullptr) {
			continue;
		}
		const uint32_t count = std::min<uint32_t>(counts[s], 32);
		bool           same  = true;
		for (uint32_t i = 0; i < count; i++) {
			if (record->user[s][i] != sgprs[s][i]) {
				g_user_diff[s][i].fetch_add(1, std::memory_order_relaxed);
				same = false;
				// A sample: the first mismatches of each SGPR, with both values.
				static std::array<std::array<std::atomic<uint32_t>, 32>, 2> logged {};
				if (logged[s][i].fetch_add(1, std::memory_order_relaxed) < 4) {
					Log::WriteUnmuted(fmt::format(
					    "Native records N3 user data: {} stage SGPR {}: engine 0x{:08x}, Kyty "
					    "0x{:08x} (SGPR {} 0x{:08x} / 0x{:08x})\n",
					    s == 0 ? "vertex" : "pixel", i, record->user[s][i], sgprs[s][i], i ^ 1u,
					    record->user[s][i ^ 1u], sgprs[s][i ^ 1u]));
				}
			}
		}
		g_user_stages.fetch_add(1, std::memory_order_relaxed);
		g_user_sgprs.fetch_add(count, std::memory_order_relaxed);
		const uint32_t type  = std::min<uint32_t>(record->user_type[s], 8);
		const uint32_t index = std::min<uint32_t>(record->user_stage[s], 5);
		g_user_by_type[type][index].fetch_add(1, std::memory_order_relaxed);
		if (same) {
			g_user_stages_same.fetch_add(1, std::memory_order_relaxed);
			g_user_same_by_type[type][index].fetch_add(1, std::memory_order_relaxed);
		}
	}
	for (uint32_t s = 0; s < 2; s++) {
		const auto& stage = record->stages[s];
		if (stage.size == 0) {
			continue;
		}
		g_n3_stages.fetch_add(1, std::memory_order_relaxed);
		if (sgprs[s] == nullptr || stage.slot + 2 > counts[s]) {
			g_n3_no_slot.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		const uint64_t vsharp_base =
		    (sgprs[s][stage.slot] | (static_cast<uint64_t>(sgprs[s][stage.slot + 1]) << 32u)) &
		    0x0000FFFFFFFFFFFFull;
		if (vsharp_base == stage.guest) {
			g_n3_address.fetch_add(1, std::memory_order_relaxed);
		}
		if (std::memcmp(base + stage.offset, reinterpret_cast<const void*>(stage.guest),
		                stage.size) == 0) {
			g_n3_bytes.fetch_add(1, std::memory_order_relaxed);
		}
	}
}

// Step 2, GPU thread: compared stages, and how they compared.
std::atomic<uint64_t> g_walk_compared {0}, g_walk_equal {0}, g_walk_differ {0},
    g_walk_none {0}, g_walk_failed {0};

void ReportWalks() {
	static std::array<uint64_t, 9> last {};
	const std::array<uint64_t, 9>  now = {g_walks.load(),          g_walk_ns.load(),
	                                      g_walk_unreadable.load(), g_walk_unknown.load(),
	                                      g_walk_compared.load(),   g_walk_equal.load(),
	                                      g_walk_differ.load(),     g_walk_none.load(),
	                                      g_walk_failed.load()};
	std::array<double, 9> d {};
	for (size_t i = 0; i < now.size(); i++) {
		d[i]    = static_cast<double>(now[i] - last[i]);
		last[i] = now[i];
	}
	const double compared = std::max(1.0, d[4]);
	static std::array<uint64_t, 4> last_why {};
	std::array<uint64_t, 4>        why {};
	for (size_t i = 0; i < why.size(); i++) {
		const uint64_t now_why = g_walk_why[i].load();
		why[i]                 = now_why - last_why[i];
		last_why[i]            = now_why;
	}
	Log::WriteUnmuted(fmt::format(
	    "Native records N3 walks failed: {} more user data than 32, {} a read refused, {} otherwise\n",
	    why[1], why[2], why[3]));
	static std::array<uint64_t, 4> last_reads {};
	std::array<uint64_t, 4>        reads {};
	Libs::Graphics::Gt7NativeWalk::ReadStats(&reads[0], &reads[1], &reads[2], &reads[3]);
	const double read_count = static_cast<double>(std::max<uint64_t>(reads[0] - last_reads[0], 1));
	Log::WriteUnmuted(fmt::format(
	    "Native records N3 walk reads: {:.1f} a walk, {:.2f} us each ({:.2f} us of it in refused "
	    "probes), {:.1f}% from the backing store after a refused probe\n",
	    d[0] == 0 ? 0.0 : read_count / d[0],
	    static_cast<double>(reads[1] - last_reads[1]) / 1000.0 / read_count,
	    static_cast<double>(reads[2] - last_reads[2]) / 1000.0 / read_count,
	    100.0 * static_cast<double>(reads[3] - last_reads[3]) / read_count));
	last_reads = reads;
	Log::WriteUnmuted(fmt::format(
	    "Native records N3 walks (10 s): {:.0f} stages walked on the render threads, {:.2f} us each, "
	    "{:.0f} unreadable, {:.0f} with no program learned | GPU thread: {:.0f} stages compared: "
	    "{:.2f}% the same resources, {:.2f}% different, {:.2f}% not walked, {:.2f}% walk failed\n",
	    d[0], d[0] == 0 ? 0.0 : d[1] / 1000.0 / d[0], d[2], d[3], d[4], 100.0 * d[5] / compared,
	    100.0 * d[6] / compared, 100.0 * d[7] / compared, 100.0 * d[8] / compared));
}

void ReportRecords() {
	static std::array<uint64_t, 9> last {};
	const std::array<uint64_t, 9>  now = {g_records_made.load(), g_records_ns.load(),
	                                      g_record_bytes.load(), g_n3_draws.load(),
	                                      g_n3_stale.load(),     g_n3_stages.load(),
	                                      g_n3_address.load(),   g_n3_bytes.load(),
	                                      g_n3_no_slot.load()};
	std::array<double, 9> d {};
	for (size_t i = 0; i < now.size(); i++) {
		d[i]    = static_cast<double>(now[i] - last[i]);
		last[i] = now[i];
	}
	const double stages = std::max(1.0, d[5]);
	static uint64_t last_user[3] {};
	const uint64_t  user_now[3] = {g_user_stages.load(), g_user_stages_same.load(),
	                               g_user_sgprs.load()};
	const double    user_stages = static_cast<double>(user_now[0] - last_user[0]);
	const double    user_same   = static_cast<double>(user_now[1] - last_user[1]);
	const double    user_sgprs  = static_cast<double>(user_now[2] - last_user[2]);
	std::copy(std::begin(user_now), std::end(user_now), std::begin(last_user));
	static std::array<std::array<uint64_t, 32>, 2> last_diff {};
	std::string                                    diffs;
	for (uint32_t s = 0; s < 2; s++) {
		for (uint32_t i = 0; i < 32; i++) {
			const uint64_t now_diff = g_user_diff[s][i].load();
			if (now_diff != last_diff[s][i]) {
				diffs += fmt::format(" {}{}:{}", s == 0 ? "v" : "p", i, now_diff - last_diff[s][i]);
			}
			last_diff[s][i] = now_diff;
		}
	}
	static std::array<std::array<uint64_t, 6>, 9> last_by {}, last_same_by {};
	std::string                                   by_type;
	for (uint32_t t = 0; t < 9; t++) {
		for (uint32_t i = 0; i < 6; i++) {
			const uint64_t all  = g_user_by_type[t][i].load();
			const uint64_t same = g_user_same_by_type[t][i].load();
			if (all != last_by[t][i]) {
				by_type += fmt::format(" type {} stage {}: {} ({:.1f}% same)", t, i, all - last_by[t][i],
				                       100.0 * static_cast<double>(same - last_same_by[t][i]) /
				                           static_cast<double>(all - last_by[t][i]));
			}
			last_by[t][i]      = all;
			last_same_by[t][i] = same;
		}
	}
	Log::WriteUnmuted(fmt::format("Native records N3 user data by shader type and GT7 stage:{}\n",
	                              by_type.empty() ? " none" : by_type));
	Log::WriteUnmuted(fmt::format(
	    "Native records N3 user data (10 s): {:.0f} stages compared, {:.2f}% with every SGPR as "
	    "the engine's shadow, {:.1f} SGPRs each | differing SGPRs (stage, index: count):{}\n",
	    user_stages, user_stages == 0 ? 0.0 : 100.0 * user_same / user_stages,
	    user_stages == 0 ? 0.0 : user_sgprs / user_stages, diffs.empty() ? " none" : diffs));
	Log::WriteUnmuted(fmt::format(
	    "Native records N3 (10 s): {:.0f} made on the render threads, {:.2f} us and {:.0f} bytes "
	    "of constants each | GPU thread: {:.0f} marked draws with one, {:.0f} stale | {:.0f} "
	    "stages with constants: descriptor at the block {:.2f}%, bytes as copied {:.2f}%, no "
	    "slot {:.0f}\n",
	    d[0], d[0] == 0 ? 0.0 : d[1] / 1000.0 / d[0], d[0] == 0 ? 0.0 : d[2] / d[0], d[3], d[4],
	    d[5], 100.0 * d[6] / stages, 100.0 * d[7] / stages, d[8]));
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
	t_ctx            = ctx;
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
		if (RecordsEnabled()) {
			ReportRecords();
		}
		if (RecordsWalkEnabled()) {
			ReportWalks();
		}
		if (RecordsUseEnabled()) {
			ReportUse();
			ReportImages();
			ReportBuffers();
			ReportDcbBuffers();
		}
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
	if (RecordsEnabled()) {
		identity->record = MakeRecord();
		if (identity->record != 0) {
			identity->sequence = static_cast<uint32_t>(
			    reinterpret_cast<const Record*>(identity->record)->serial);
		}
	}
	g_markers_written.fetch_add(1, std::memory_order_relaxed);
	return true;
}

void WriteMarker(uint32_t* packet, const Identity& identity) {
	packet[0] = KYTY_PM4(MarkerDwords, Pm4::IT_NOP, Pm4::R_GT7_DRAW);
	const uint64_t fields[7] = {identity.set,  identity.material, identity.block, identity.shaders,
	                            identity.mesh, identity.state,    identity.record};
	for (int i = 0; i < 7; i++) {
		packet[1 + i * 2] = static_cast<uint32_t>(fields[i]);
		packet[2 + i * 2] = static_cast<uint32_t>(fields[i] >> 32u);
	}
	packet[15] = identity.sequence;
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
	g_pending_identity = {field(0), field(1), field(2), field(3),
	                      field(4), field(5), field(6), payload[14]};
	g_pending          = true;
}

void OnDraw(const uint32_t* ps_user_sgprs, uint32_t count, const uint32_t* gs_user_sgprs,
            uint32_t gs_count) {
	g_draws.fetch_add(1, std::memory_order_relaxed);
	Mutations::Reset();
	if (!g_pending) {
		return;
	}
	g_pending = false;
	const auto& id = g_pending_identity;
	if (id.record != 0 && RecordsCheck()) {
		// The serial is the record's (it went into the marker's sequence dword).
		const uint32_t* sgprs[2]  = {gs_user_sgprs, ps_user_sgprs};
		const uint32_t  counts[2] = {gs_count, count};
		const auto*     record    = reinterpret_cast<const Record*>(id.record);
		CheckRecord(id.record,
		            (record->serial & ~uint64_t {0xffffffff}) | id.sequence, sgprs, counts);
	}
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
	if (RecordsUseEnabled()) {
		FreeRecordStages();
	}
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

bool RecordsWalk() {
	return RecordsWalkEnabled();
}

bool RecordsCheck() {
	static const bool enabled = [] {
		const char* check   = std::getenv("KYTY_GT7_NATIVE_CHECK");
		const char* records = std::getenv("KYTY_GT7_NATIVE_RECORDS");
		return (check != nullptr && check[0] == '1') ||
		       (records != nullptr && std::strcmp(records, "walk") == 0);
	}();
	return enabled;
}

bool RecordsUse() {
	return RecordsUseEnabled();
}

// The current marked draw's record, when it is still the record the marker named.
Record* CurrentRecord() {
	const auto& id = g_current_identity;
	if (!g_current_marked || id.record == 0) {
		return nullptr;
	}
	auto* record = reinterpret_cast<Record*>(id.record);
	return static_cast<uint32_t>(record->serial) == id.sequence ? record : nullptr;
}

std::atomic<uint64_t> g_use_offered {0}, g_use_taken {0}, g_use_used {0};

Libs::Graphics::DrawPrep::StageResult* TakeRecordStage(uint32_t stage) {
	auto* record = CurrentRecord();
	if (record == nullptr) {
		return nullptr;
	}
	const uint32_t index =
	    stage == static_cast<uint32_t>(Libs::Graphics::ShaderType::Pixel) ? 1u : 0u;
	auto* prepared = record->prepared[index];
	if (prepared == nullptr || !prepared->ok || prepared->stage != stage) {
		return nullptr;
	}
	g_use_taken.fetch_add(1, std::memory_order_relaxed);
	return prepared;
}

// After the draw: the record's prepared stages are freed, counted by whether the draw used them.
void FreeRecordStages() {
	auto* record = CurrentRecord();
	if (record == nullptr) {
		return;
	}
	delete record->image_bindings;
	record->image_bindings = nullptr;
	for (auto*& prepared: record->prepared) {
		if (prepared == nullptr) {
			continue;
		}
		g_use_offered.fetch_add(1, std::memory_order_relaxed);
		if (prepared->consumed) {
			g_use_used.fetch_add(1, std::memory_order_relaxed);
		}
		delete prepared;
		prepared = nullptr;
	}
}

const void* RecordImageBinding(uint32_t i) {
	const auto* record = CurrentRecord();
	if (record == nullptr || record->image_bindings == nullptr || i >= 32) {
		return nullptr;
	}
	return (*record->image_bindings)[i].get();
}

uint64_t CurrentLearnedKey(uint32_t stage) {
	const auto& id = g_current_identity;
	return g_current_marked && id.state != 0 ? LearnedKey(id.state, id.material, stage) : 0;
}

// N4: images compared, the same, another, and why the render thread had none.
std::atomic<uint64_t> g_n4_draws {0}, g_n4_no_record {0}, g_n4_images {0}, g_n4_same {0},
    g_n4_other {0}, g_n4_no_desc {0}, g_n4_not_found {0}, g_n4_not_learned {0};
// Coverage: marked pixel stages whose every image the record named right (a whole draw a record
// could carry), and those with one image it could not name.
std::atomic<uint64_t> g_n4_all_named {0}, g_n4_one_missing {0};
std::atomic<uint64_t> g_n4_by_binding {0}; // of the same, named by the GPU thread's binding

void CompareRecordImages(const uint32_t* image_ids, uint32_t count) {
	g_n4_draws.fetch_add(1, std::memory_order_relaxed);
	auto* record = CurrentRecord();
	if (record == nullptr) {
		g_n4_no_record.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if (record->image_count == 0) {
		g_n4_not_learned.fetch_add(count, std::memory_order_relaxed);
		return;
	}
	uint32_t named = 0;
	for (uint32_t i = 0; i < count; i++) {
		named += i < record->image_count &&
		                 (record->image_states[i] == 1 || record->image_states[i] == 4) &&
		                 record->image_ids[i] == image_ids[i]
		             ? 1u
		             : 0u;
	}
	if (named == count) {
		g_n4_all_named.fetch_add(1, std::memory_order_relaxed);
	} else if (named + 1 == count) {
		g_n4_one_missing.fetch_add(1, std::memory_order_relaxed);
	}
	for (uint32_t i = 0; i < count; i++) {
		g_n4_images.fetch_add(1, std::memory_order_relaxed);
		if (i >= record->image_count || record->image_states[i] == 2) {
			g_n4_no_desc.fetch_add(1, std::memory_order_relaxed);
		} else if (record->image_states[i] == 3) {
			g_n4_not_found.fetch_add(1, std::memory_order_relaxed);
		} else if (record->image_ids[i] == image_ids[i]) {
			g_n4_same.fetch_add(1, std::memory_order_relaxed);
			if (record->image_states[i] == 4) {
				g_n4_by_binding.fetch_add(1, std::memory_order_relaxed);
			}
		} else {
			g_n4_other.fetch_add(1, std::memory_order_relaxed);
		}
	}
}

// N4 buffers, by stage: stages compared, every buffer named right, buffers, the same, another,
// none found, and stages whose program was not learned yet.
std::array<std::array<std::atomic<uint64_t>, 7>, 2> g_n4_buffers {};

void CompareRecordBuffers(uint32_t stage, const uint32_t* buffer_ids, uint32_t count) {
	auto* record = CurrentRecord();
	if (record == nullptr || stage > 1) {
		return;
	}
	auto& c = g_n4_buffers[stage];
	if (record->buffer_count[stage] == 0 && count != 0) {
		c[6].fetch_add(1, std::memory_order_relaxed);
		return;
	}
	c[0].fetch_add(1, std::memory_order_relaxed);
	uint32_t named = 0;
	for (uint32_t i = 0; i < count; i++) {
		c[2].fetch_add(1, std::memory_order_relaxed);
		const uint8_t  state = i < record->buffer_count[stage] ? record->buffer_states[stage][i] : 3;
		const uint32_t id    = state == 0 ? UINT32_MAX : record->buffer_ids[stage][i];
		if (state == 3) {
			c[5].fetch_add(1, std::memory_order_relaxed);
		} else if (id == buffer_ids[i]) {
			c[3].fetch_add(1, std::memory_order_relaxed);
			named++;
		} else {
			c[4].fetch_add(1, std::memory_order_relaxed);
		}
	}
	if (named == count) {
		c[1].fetch_add(1, std::memory_order_relaxed);
	}
}

void ReportBuffers() {
	static std::array<std::array<uint64_t, 7>, 2> last {};
	std::string                                   text;
	for (uint32_t s = 0; s < 2; s++) {
		std::array<double, 7> d {};
		for (size_t i = 0; i < d.size(); i++) {
			const uint64_t now = g_n4_buffers[s][i].load();
			d[i]               = static_cast<double>(now - last[s][i]);
			last[s][i]         = now;
		}
		const double stages  = std::max(1.0, d[0]);
		const double buffers = std::max(1.0, d[2]);
		text += fmt::format(
		    " | {} stages {:.0f} ({:.0f} not learned): every buffer named {:.1f}%; {:.0f} buffers: "
		    "the same {:.2f}%, another {:.2f}%, none found {:.2f}%",
		    s == 0 ? "vertex" : "pixel", d[0], d[6], 100.0 * d[1] / stages, d[2],
		    100.0 * d[3] / buffers, 100.0 * d[4] / buffers, 100.0 * d[5] / buffers);
	}
	Log::WriteUnmuted(fmt::format("Native records N4 buffers (10 s){}\n", text));
}

void ReportImages() {
	static std::array<uint64_t, 10> last {};
	const std::array<uint64_t, 10>  now = {g_n4_draws.load(),   g_n4_no_record.load(),
	                                       g_n4_images.load(),  g_n4_same.load(),
	                                       g_n4_other.load(),   g_n4_no_desc.load(),
	                                       g_n4_not_found.load(), g_n4_not_learned.load(),
	                                       g_n4_all_named.load(), g_n4_one_missing.load()};
	std::array<double, 10>          d {};
	for (size_t i = 0; i < now.size(); i++) {
		d[i]    = static_cast<double>(now[i] - last[i]);
		last[i] = now[i];
	}
	const double images = std::max(1.0, d[2]);
	Log::WriteUnmuted(fmt::format(
	    "Native records N4 images (10 s): {:.0f} marked pixel stages ({:.0f} without a record), "
	    "{:.0f} images: {:.2f}% the image the GPU thread bound, {:.2f}% another, {:.2f}% no "
	    "description learned, {:.2f}% no image with that backing | {:.0f} images of programs not "
	    "learned yet | pixel stages with every image named: {:.1f}%, all but one: {:.1f}% | named by "
	    "the GPU thread's binding: {:.2f}%\n",
	    d[0], d[1], d[2], 100.0 * d[3] / images, 100.0 * d[4] / images, 100.0 * d[5] / images,
	    100.0 * d[6] / images, d[7], 100.0 * d[8] / std::max(1.0, d[0] - d[1]),
	    100.0 * d[9] / std::max(1.0, d[0] - d[1]),
	    100.0 * static_cast<double>(g_n4_by_binding.exchange(0)) / images));
}

void ReportUse() {
	static std::array<uint64_t, 3> last {};
	const std::array<uint64_t, 3>  now = {g_use_offered.load(), g_use_taken.load(),
	                                      g_use_used.load()};
	std::array<double, 3>          d {};
	for (size_t i = 0; i < now.size(); i++) {
		d[i]    = static_cast<double>(now[i] - last[i]);
		last[i] = now[i];
	}
	const double offered = std::max(1.0, d[0]);
	Log::WriteUnmuted(fmt::format(
	    "Native records N3 use (10 s): {:.0f} stages prepared on the render threads reached their "
	    "draw, {:.1f}% taken by the GPU thread, {:.1f}% used in place of its walk\n",
	    d[0], 100.0 * d[1] / offered, 100.0 * d[2] / offered));
}

void OnStageWalk(uint32_t stage, const Libs::Graphics::Gt7NativeWalk::Learned& learned,
                 uint64_t hash) {
	const auto& id = g_current_identity;
	if (id.state == 0 || stage > 1) {
		return;
	}
	const uint64_t key = LearnedKey(id.state, id.material, stage);
	bool           known = false;
	{
		std::shared_lock lock(g_learned_mutex);
		const auto       it = g_learned.find(key);
		known = it != g_learned.end() && it->second.entry == learned.entry &&
		        it->second.shader_base == learned.shader_base &&
		        it->second.user_data_count == learned.user_data_count;
	}
	if (!known) {
		std::unique_lock lock(g_learned_mutex);
		if (g_learned.size() > (1u << 18u)) {
			g_learned.clear();
		}
		g_learned[key] = learned;
	}
	if (id.record == 0 || !RecordsCheck()) {
		return;
	}
	const auto* record = reinterpret_cast<const Record*>(id.record);
	if (static_cast<uint32_t>(record->serial) != id.sequence) {
		return;
	}
	g_walk_compared.fetch_add(1, std::memory_order_relaxed);
	switch (record->walk_state[stage]) {
		case 0: g_walk_none.fetch_add(1, std::memory_order_relaxed); break;
		case 2: g_walk_failed.fetch_add(1, std::memory_order_relaxed); break;
		default:
			(record->walk_hash[stage] == hash ? g_walk_equal : g_walk_differ)
			    .fetch_add(1, std::memory_order_relaxed);
			break;
	}
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
