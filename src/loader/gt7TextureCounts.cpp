#include "loader/gt7TextureCounts.h"

#include "common/abi.h"
#include "common/guestWatch.h"
#include "common/logging/log.h"
#include "common/perfStats.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "loader/gt7GuestHook.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

namespace Loader::Gt7TextureCounts {

namespace {

constexpr uint64_t Relocate     = 0x688100 + 0xc; // gt7_RelocateTexturePackage, after the check
constexpr uint64_t BindTexture  = 0xf4fc0;        // gt7_BindTexture
constexpr uint64_t MaterialCopy = 0x665f0;        // memory for a material's constants (gt7_BindMaterial)
constexpr uint64_t BindBuffer   = 0x1a3ceb0;      // gt7_BindBuffer (same first bytes as gt7_BindTexture)
constexpr uint64_t Commit       = 0x16370;        // gt7_CommitDrawState(ctx)
constexpr uint64_t BlockCopy    = 0x12cea;        // in gt7_BindMaterial: memcpy(dst, block, size)
constexpr uint64_t BlockCall    = 0x12cff;        // the memcpy call after the moved instructions
constexpr uint64_t RecordSize   = 0x48;
constexpr auto     ReportEvery  = std::chrono::seconds(10);

// cmp dword [rdi], 0xb5535854; je ... (the package is relocated already), then the moved part:
// push rbp; mov rbp, rsp; push r15; push r14; push r12; push rbx; movzx eax, word [rdi+0x1e]
constexpr std::array<uint8_t, 12> RelocateCheck  = {0x81, 0x3f, 0x54, 0x58, 0x53, 0xb5,
                                                    0x0f, 0x84, 0xc9, 0x00, 0x00, 0x00};
constexpr std::array<uint8_t, 15> RelocateMoved  = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                                    0x41, 0x54, 0x53, 0x0f, 0xb7, 0x47, 0x1e};
// push rbp; mov rbp, rsp; push r15; push r14; push rbx; push rax; mov r15d, edx; mov r14d, esi
constexpr std::array<uint8_t, 16> BindMoved = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                               0x53, 0x50, 0x41, 0x89, 0xd7, 0x41, 0x89, 0xf6};

// push rbp; mov rbp, rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp, 0x38
constexpr std::array<uint8_t, 17> MaterialMoved = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57,
                                                   0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                                   0x53, 0x48, 0x83, 0xec, 0x38};

// push rbp; mov rbp, rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp, 0x158
constexpr std::array<uint8_t, 20> CommitMoved = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                                 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                                 0x81, 0xec, 0x58, 0x01, 0x00, 0x00};

// mov rax, [rbp-0x90]; mov rsi, [rbp-0x98] (the material's block); mov rdi, r14 (the copy in the
// command buffer); movzx edx, word [rax+0x10] (its size), then call memcpy. Reached only by a jump
// to its start (0x900012965).
constexpr std::array<uint8_t, 21> BlockMoved = {0x48, 0x8b, 0x85, 0x70, 0xff, 0xff, 0xff,
                                                0x48, 0x8b, 0xb5, 0x68, 0xff, 0xff, 0xff,
                                                0x4c, 0x89, 0xf7, 0x0f, 0xb7, 0x50, 0x10};
constexpr std::array<uint8_t, 1>  BlockCallOp = {0xe8};

// GT7RenderContext (docs/GT7-RENDER-FRONTEND.md): six GT7StageBinding of 0xb0 bytes at +0x870,
// each with its shader at +0x0, user-data SGPRs 0-31 at +0x18 and their dirty bits at +0x98
constexpr uint64_t StagesOffset  = 0x870;
constexpr uint64_t StageSize     = 0xb0;
constexpr uint32_t StageCount    = 6;
constexpr uint32_t PointerScan   = 0x200; // bytes scanned behind a user-data pointer

// texture records by address: first -> end of each package's array
std::shared_mutex             g_records_mutex;
std::map<uint64_t, uint64_t>  g_records;
std::atomic<uint64_t>         g_packages {0};
std::atomic<uint64_t>         g_textures {0};

std::atomic<uint64_t>         g_binds {0};
std::atomic<uint64_t>         g_hits {0};
std::mutex                    g_window_mutex;
std::unordered_set<uint64_t>  g_hit_records;  // this window's
std::unordered_map<uint64_t, uint64_t> g_miss_bases; // this window's, by T# base address

// The scans: the first 8 bytes of every package T# (relocated: the reporter adds a package's
// records some seconds after it loads), to find T#s in the memory of material constant blocks and
// of bound buffers.
std::shared_mutex                      g_tsharps_mutex;
std::unordered_map<uint64_t, uint64_t> g_tsharps; // first 8 bytes -> record
std::vector<std::pair<uint64_t, uint64_t>> g_pending; // record arrays not read yet (records mutex)

constexpr uint32_t ScanLimit = 0x1000; // bytes scanned of one block at most

struct Scan {
	const char*                  label;
	std::atomic<uint64_t>        blocks {0};
	std::atomic<uint64_t>        bytes {0};
	std::atomic<uint64_t>        with {0};
	std::atomic<uint64_t>        tsharps {0};
	std::atomic<uint64_t>        skipped {0}; // not committed guest memory
	std::unordered_set<uint64_t> records;     // this window's (window mutex)
	uint64_t last_blocks = 0, last_bytes = 0, last_with = 0, last_tsharps = 0, last_skipped = 0;
};

Scan g_material_scan {"Material blocks (copied)"};
Scan g_buffer_scan {"Bound buffers (gt7_BindBuffer)"};

void ScanBlock(Scan& scan, const uint8_t* source, uint32_t size) {
	scan.blocks.fetch_add(1, std::memory_order_relaxed);
	if (source == nullptr || size < 8) {
		return;
	}
	size = std::min(size, ScanLimit);
	if (!Libs::LibKernel::Memory::IsCommittedRange(reinterpret_cast<uint64_t>(source), size)) {
		scan.skipped.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	scan.bytes.fetch_add(size, std::memory_order_relaxed);
	uint32_t                 found = 0;
	std::array<uint64_t, 64> records {};
	{
		std::shared_lock lock(g_tsharps_mutex);
		for (uint32_t offset = 0; offset + 8 <= size; offset += 4) {
			uint64_t head = 0;
			std::memcpy(&head, source + offset, sizeof(head));
			if (head == 0) {
				continue;
			}
			if (auto it = g_tsharps.find(head); it != g_tsharps.end()) {
				if (found < records.size()) {
					records[found] = it->second;
				}
				found++;
			}
		}
	}
	if (found == 0) {
		return;
	}
	scan.with.fetch_add(1, std::memory_order_relaxed);
	scan.tsharps.fetch_add(found, std::memory_order_relaxed);
	std::scoped_lock lock(g_window_mutex);
	for (uint32_t i = 0; i < std::min<uint32_t>(found, records.size()); i++) {
		scan.records.insert(records[i]);
	}
}

void ReportScan(Scan& scan, double frames) {
	size_t records = 0;
	{
		std::scoped_lock lock(g_window_mutex);
		records = scan.records.size();
		scan.records.clear();
	}
	const auto blocks  = scan.blocks.load(std::memory_order_relaxed);
	const auto bytes   = scan.bytes.load(std::memory_order_relaxed);
	const auto with    = scan.with.load(std::memory_order_relaxed);
	const auto tsharps = scan.tsharps.load(std::memory_order_relaxed);
	const auto skipped = scan.skipped.load(std::memory_order_relaxed);
	const auto scanned = static_cast<double>((blocks - scan.last_blocks) -
	                                         (skipped - scan.last_skipped));
	Log::WriteUnmuted(fmt::format(
	    "{}: a frame: {:.0f} ({:.0f} not in guest memory, {:.0f} bytes each scanned), {:.0f} "
	    "holding package T#s, {:.0f} package T#s in them ({} distinct records in the window)\n",
	    scan.label, static_cast<double>(blocks - scan.last_blocks) / frames,
	    static_cast<double>(skipped - scan.last_skipped) / frames,
	    scanned <= 0 ? 0.0 : static_cast<double>(bytes - scan.last_bytes) / scanned,
	    static_cast<double>(with - scan.last_with) / frames,
	    static_cast<double>(tsharps - scan.last_tsharps) / frames, records));
	scan.last_blocks  = blocks;
	scan.last_bytes   = bytes;
	scan.last_with    = with;
	scan.last_tsharps = tsharps;
	scan.last_skipped = skipped;
}

bool IsRecord(uint64_t address) {
	std::shared_lock lock(g_records_mutex);
	auto it = g_records.upper_bound(address);
	if (it == g_records.begin()) {
		return false;
	}
	--it;
	return address < it->second && (address - it->first) % RecordSize == 0;
}

KYTY_SYSV_ABI void OnPackage(const uint8_t* package) {
	uint16_t count = 0;
	uint64_t records = 0;
	std::memcpy(&count, package + 0x1c, sizeof(count));
	std::memcpy(&records, package + 0x20, sizeof(records));
	if (count == 0 || records == 0) {
		return;
	}
	const uint64_t end = records + count * RecordSize;
	{
		std::unique_lock lock(g_records_mutex);
		// a package loaded where an unloaded one was replaces it
		g_records.erase(g_records.lower_bound(records), g_records.lower_bound(end));
		g_records[records] = end;
		g_pending.emplace_back(records, end);
	}
	g_packages.fetch_add(1, std::memory_order_relaxed);
	g_textures.fetch_add(count, std::memory_order_relaxed);
}

KYTY_SYSV_ABI void OnBindTexture(const uint32_t* tsharp) {
	g_binds.fetch_add(1, std::memory_order_relaxed);
	const auto address = reinterpret_cast<uint64_t>(tsharp);
	if (IsRecord(address)) {
		g_hits.fetch_add(1, std::memory_order_relaxed);
		std::scoped_lock lock(g_window_mutex);
		g_hit_records.insert(address);
		return;
	}
	const uint64_t base = ((static_cast<uint64_t>(tsharp[1] & 0xffu) << 32u) | tsharp[0]) << 8u;
	std::scoped_lock lock(g_window_mutex);
	g_miss_bases[base]++;
}

// FUN_9000665f0(allocator, cache, size, source): `source` is the material's constant block
KYTY_SYSV_ABI void OnMaterialBlock(const void* /*allocator*/, const void* /*cache*/,
                                   uint32_t size, const uint8_t* source) {
	ScanBlock(g_material_scan, source, size);
}

// The commit scan: per draw, in each stage whose user data changed, package T#s in the SGPRs
// themselves and behind SGPR pairs that hold a guest pointer.
std::atomic<uint64_t>                        g_commits {0};
std::array<std::atomic<uint64_t>, StageCount> g_stage_changed {};
std::array<std::atomic<uint64_t>, StageCount> g_direct {};
std::array<std::atomic<uint64_t>, StageCount> g_via_pointer {};
std::unordered_map<uint32_t, uint64_t>       g_direct_slots;  // stage << 8 | sgpr (window mutex)
std::unordered_map<uint64_t, uint64_t>       g_pointer_slots; // stage << 40 | sgpr << 32 | offset
std::unordered_set<uint64_t>                 g_commit_records;

// The write watch: once a race is on (WatchAfter package T#s seen behind stage 1's SGPRs 2-3),
// the first such context and the table those SGPRs point at; the reporter thread arms hardware
// write watches on both (Common::GuestWatch does not watch the thread that arms).
constexpr uint64_t     WatchAfter   = 50000;
constexpr uint64_t     WatchedSgprs = StagesOffset + StageSize + 0x18 + 2 * 4; // ctx+0x940
std::atomic<uint64_t>  g_stage1_table_tsharps {0};
std::atomic<uint64_t>  g_watch_ctx {0};
std::atomic<uint64_t>  g_watch_table {0};
bool                   g_watch_armed = false; // reporter thread only

uint32_t CountTsharps(const uint8_t* bytes, uint32_t size, uint32_t step,
                      std::vector<std::pair<uint32_t, uint64_t>>* hits) {
	uint32_t found = 0;
	for (uint32_t offset = 0; offset + 8 <= size; offset += step) {
		uint64_t head = 0;
		std::memcpy(&head, bytes + offset, sizeof(head));
		if (head == 0) {
			continue;
		}
		if (auto it = g_tsharps.find(head); it != g_tsharps.end()) {
			hits->emplace_back(offset, it->second);
			found++;
		}
	}
	return found;
}

KYTY_SYSV_ABI void OnCommit(const uint8_t* ctx) {
	g_commits.fetch_add(1, std::memory_order_relaxed);
	std::vector<std::pair<uint32_t, uint64_t>> direct;
	std::vector<std::tuple<uint32_t, uint32_t, uint32_t, uint64_t>> pointed; // stage, sgpr, offset
	for (uint32_t s = 0; s < StageCount; s++) {
		const uint8_t* stage  = ctx + StagesOffset + s * StageSize;
		uint64_t       shader = 0;
		uint64_t       dirty  = 0;
		std::memcpy(&shader, stage, sizeof(shader));
		std::memcpy(&dirty, stage + 0x98, sizeof(dirty));
		if (shader == 0 || (dirty & 0xffffffffu) == 0) {
			continue;
		}
		g_stage_changed[s].fetch_add(1, std::memory_order_relaxed);
		std::array<uint32_t, 32> user {};
		std::memcpy(user.data(), stage + 0x18, sizeof(user));
		direct.clear();
		uint32_t via = 0;
		{
			std::shared_lock lock(g_tsharps_mutex);
			const auto found = CountTsharps(reinterpret_cast<const uint8_t*>(user.data()),
			                                sizeof(user), 4, &direct);
			if (found != 0) {
				g_direct[s].fetch_add(found, std::memory_order_relaxed);
			}
			for (uint32_t i = 0; i + 1 < user.size(); i++) {
				const uint64_t pointer = user[i] | (static_cast<uint64_t>(user[i + 1]) << 32u);
				// guest memory lies at 0x8_0000_0000 and up; descriptors are 16-byte aligned
				if (user[i + 1] < 0x8 || user[i + 1] > 0x1ff || (pointer & 0xf) != 0 ||
				    !Libs::LibKernel::Memory::IsCommittedRange(pointer, PointerScan)) {
					continue;
				}
				std::vector<std::pair<uint32_t, uint64_t>> hits;
				via += CountTsharps(reinterpret_cast<const uint8_t*>(pointer), PointerScan, 16,
				                    &hits);
				if (s == 1 && i == 2 && !hits.empty() &&
				    g_stage1_table_tsharps.fetch_add(hits.size(), std::memory_order_relaxed) >
				        WatchAfter) {
					uint64_t none = 0;
					if (g_watch_ctx.compare_exchange_strong(none, reinterpret_cast<uint64_t>(ctx))) {
						g_watch_table.store(pointer, std::memory_order_release);
					}
				}
				for (const auto& [offset, record]: hits) {
					pointed.emplace_back(s, i, offset, record);
				}
			}
		}
		if (via != 0) {
			g_via_pointer[s].fetch_add(via, std::memory_order_relaxed);
		}
		if (!direct.empty()) {
			std::scoped_lock lock(g_window_mutex);
			for (const auto& [offset, record]: direct) {
				g_direct_slots[s << 8u | offset / 4]++;
				g_commit_records.insert(record);
			}
		}
	}
	if (!pointed.empty()) {
		std::scoped_lock lock(g_window_mutex);
		for (const auto& [s, sgpr, offset, record]: pointed) {
			g_pointer_slots[static_cast<uint64_t>(s) << 40u | static_cast<uint64_t>(sgpr) << 32u |
			                offset]++;
			g_commit_records.insert(record);
		}
	}
}

void ReportCommits(double frames) {
	static uint64_t                               last_commits = 0;
	static std::array<uint64_t, StageCount>       last_changed {}, last_direct {}, last_via {};
	std::vector<std::pair<uint32_t, uint64_t>>    direct_slots;
	std::vector<std::pair<uint64_t, uint64_t>>    pointer_slots;
	size_t                                        records = 0;
	{
		std::scoped_lock lock(g_window_mutex);
		direct_slots.assign(g_direct_slots.begin(), g_direct_slots.end());
		pointer_slots.assign(g_pointer_slots.begin(), g_pointer_slots.end());
		records = g_commit_records.size();
		g_direct_slots.clear();
		g_pointer_slots.clear();
		g_commit_records.clear();
	}
	const auto commits = g_commits.load(std::memory_order_relaxed);
	std::string stages;
	for (uint32_t s = 0; s < StageCount; s++) {
		const auto changed = g_stage_changed[s].load(std::memory_order_relaxed);
		const auto direct  = g_direct[s].load(std::memory_order_relaxed);
		const auto via     = g_via_pointer[s].load(std::memory_order_relaxed);
		stages += fmt::format(" | stage {}: {:.0f} changed, {:.0f} in SGPRs, {:.0f} behind pointers",
		                      s, static_cast<double>(changed - last_changed[s]) / frames,
		                      static_cast<double>(direct - last_direct[s]) / frames,
		                      static_cast<double>(via - last_via[s]) / frames);
		last_changed[s] = changed;
		last_direct[s]  = direct;
		last_via[s]     = via;
	}
	std::ranges::sort(direct_slots, [](const auto& a, const auto& b) { return a.second > b.second; });
	std::ranges::sort(pointer_slots, [](const auto& a, const auto& b) { return a.second > b.second; });
	std::string busiest;
	for (size_t i = 0; i < std::min<size_t>(direct_slots.size(), 6); i++) {
		busiest += fmt::format(" s{}:sgpr{} {:.0f}", direct_slots[i].first >> 8u,
		                       direct_slots[i].first & 0xffu,
		                       static_cast<double>(direct_slots[i].second) / frames);
	}
	busiest += " | behind pointers (stage:sgpr+offset):";
	for (size_t i = 0; i < std::min<size_t>(pointer_slots.size(), 6); i++) {
		const auto key = pointer_slots[i].first;
		busiest += fmt::format(" s{}:sgpr{}+0x{:x} {:.0f}", key >> 40u, (key >> 32u) & 0xffu,
		                       key & 0xffffffffu,
		                       static_cast<double>(pointer_slots[i].second) / frames);
	}
	Log::WriteUnmuted(fmt::format(
	    "Commit scan: a frame: {:.0f} commits, package T#s by stage{} | {} distinct records in the "
	    "window | busiest in SGPRs:{}\n",
	    static_cast<double>(commits - last_commits) / frames, stages, records, busiest));
	last_commits = commits;
}

// The load watch (KYTY_TEXTURE_WATCH=material): T# slots of material blocks seen in a race; the
// reporter arms value-filtered watches on them (Common::GuestWatch::ArmExpecting), so loading the
// next race names the code that writes a material's T#s.
Scan                  g_copy_scan {"Material blocks (gt7_BindMaterial copy)"};
constexpr uint32_t    WatchSlots = 4;
std::mutex            g_slots_mutex;
std::vector<uint64_t> g_slot_blocks; // the blocks the slots are in, one slot each
std::vector<uint64_t> g_slots;
bool                  g_slots_armed = false; // reporter thread only

bool WatchMode(const char* mode) {
	static const char* value = std::getenv("KYTY_TEXTURE_WATCH");
	return value != nullptr && std::strcmp(value, mode) == 0;
}

KYTY_SYSV_ABI void OnBlockCopy(void* /*dst*/, const uint8_t* block, uint32_t size) {
	ScanBlock(g_copy_scan, block, size);
	if (!WatchMode("material") || block == nullptr) {
		return;
	}
	{
		std::scoped_lock lock(g_slots_mutex);
		if (g_slots.size() >= WatchSlots ||
		    std::ranges::find(g_slot_blocks, reinterpret_cast<uint64_t>(block)) !=
		        g_slot_blocks.end()) {
			return;
		}
	}
	size = std::min(size, ScanLimit);
	if (!Libs::LibKernel::Memory::IsCommittedRange(reinterpret_cast<uint64_t>(block), size)) {
		return;
	}
	std::vector<std::pair<uint32_t, uint64_t>> hits;
	{
		std::shared_lock lock(g_tsharps_mutex);
		CountTsharps(block, size, 8, &hits);
	}
	if (hits.empty()) {
		return;
	}
	std::scoped_lock lock(g_slots_mutex);
	if (g_slots.size() < WatchSlots) {
		g_slot_blocks.push_back(reinterpret_cast<uint64_t>(block));
		g_slots.push_back(reinterpret_cast<uint64_t>(block) + hits.front().first);
	}
}

// gt7_BindBuffer(vsharp, stage, index): the memory the V# describes (most often a material's
// persistent constants)
KYTY_SYSV_ABI void OnBindBuffer(const uint32_t* vsharp) {
	const uint64_t base   = vsharp[0] | (static_cast<uint64_t>(vsharp[1] & 0xffffu) << 32u);
	const uint32_t stride = (vsharp[1] >> 16u) & 0x3fffu;
	const uint64_t size   = stride == 0 ? vsharp[2] : static_cast<uint64_t>(stride) * vsharp[2];
	ScanBlock(g_buffer_scan, reinterpret_cast<const uint8_t*>(base),
	          static_cast<uint32_t>(std::min<uint64_t>(size, ScanLimit)));
}

// Reads the T#s of the packages loaded since the last call (relocated by now).
void AddPendingTsharps() {
	std::vector<std::pair<uint64_t, uint64_t>> pending;
	{
		std::unique_lock lock(g_records_mutex);
		pending.swap(g_pending);
	}
	std::unique_lock lock(g_tsharps_mutex);
	for (const auto& [first, end]: pending) {
		for (uint64_t record = first; record < end; record += RecordSize) {
			uint64_t head = 0;
			std::memcpy(&head, reinterpret_cast<const void*>(record), sizeof(head));
			if (head != 0) {
				g_tsharps[head] = record;
			}
		}
	}
}

void Report() {
	uint64_t last_binds    = 0;
	uint64_t last_hits     = 0;
	uint64_t last_presents = Common::PerfStats::PresentsTotal();
	auto     last_time     = std::chrono::steady_clock::now();
	for (;;) {
		std::this_thread::sleep_for(ReportEvery);
		AddPendingTsharps();
		size_t                                     records = 0;
		std::vector<std::pair<uint64_t, uint64_t>> misses;
		{
			std::scoped_lock lock(g_window_mutex);
			records = g_hit_records.size();
			misses.assign(g_miss_bases.begin(), g_miss_bases.end());
			g_hit_records.clear();
			g_miss_bases.clear();
		}
		size_t known = 0;
		{
			std::shared_lock lock(g_tsharps_mutex);
			known = g_tsharps.size();
		}
		const auto binds    = g_binds.load(std::memory_order_relaxed);
		const auto hits     = g_hits.load(std::memory_order_relaxed);
		const auto presents = Common::PerfStats::PresentsTotal();
		const auto time     = std::chrono::steady_clock::now();
		const auto frames   = static_cast<double>(std::max<uint64_t>(presents - last_presents, 1));
		const auto window_binds = static_cast<double>(binds - last_binds);
		const auto window_hits  = static_cast<double>(hits - last_hits);
		std::ranges::sort(misses, [](const auto& a, const auto& b) { return a.second > b.second; });
		std::string busiest;
		for (size_t i = 0; i < std::min<size_t>(misses.size(), 6); i++) {
			busiest += fmt::format(" 0x{:x} {:.0f}", misses[i].first,
			                       static_cast<double>(misses[i].second) / frames);
		}
		Log::WriteUnmuted(fmt::format(
		    "Texture binds: {:.1f} s, {:.0f} frames, {} packages ({} textures, {} T#s known) so far "
		    "| a frame: "
		    "{:.0f} gt7_BindTexture, {:.1f}% from package records ({} distinct records in the "
		    "window), {:.0f} from elsewhere ({} distinct textures; busiest a frame:{})\n",
		    std::chrono::duration<double>(time - last_time).count(), frames,
		    g_packages.load(std::memory_order_relaxed), g_textures.load(std::memory_order_relaxed),
		    known, window_binds / frames, window_binds == 0 ? 0.0 : 100.0 * window_hits / window_binds,
		    records, (window_binds - window_hits) / frames, misses.size(), busiest));
		ReportScan(g_material_scan, frames);
		ReportScan(g_buffer_scan, frames);
		ReportCommits(frames);
		ReportScan(g_copy_scan, frames);
		if (std::vector<uint64_t> slots; !g_slots_armed && WatchMode("material")) {
			{
				std::scoped_lock lock(g_slots_mutex);
				if (g_slots.size() >= WatchSlots) {
					slots = g_slots;
				}
			}
			if (!slots.empty()) {
				g_slots_armed  = true;
				uint32_t armed = 0;
				for (const auto slot: slots) {
					armed += Common::GuestWatch::ArmExpecting(slot, "GT7 material block T# slot") != 0
					             ? 1
					             : 0;
				}
				std::string list;
				for (const auto slot: slots) {
					list += fmt::format(" 0x{:x}", slot);
				}
				Log::WriteUnmuted(fmt::format("Material blocks: watching T# slots{} ({} armed); load "
				                              "another race to see what writes them\n",
				                              list, armed));
			}
		}
		if (const auto table = g_watch_table.load(std::memory_order_acquire);
		    !g_watch_armed && table != 0 && WatchMode("commit")) {
			g_watch_armed = true;
			const auto     ctx         = g_watch_ctx.load(std::memory_order_relaxed);
			const uint64_t watched[2]  = {ctx + WatchedSgprs, table};
			const auto     threads     = Common::GuestWatch::Arm(
			    watched, 2, "GT7 stage 1 SGPRs 2-3 (texture table pointer) and the table's first T#");
			Log::WriteUnmuted(fmt::format("Commit scan: watching ctx 0x{:x} +0x{:x} and table 0x{:x} "
			                              "on {} threads\n",
			                              ctx, WatchedSgprs, table, threads));
		}
		last_binds    = binds;
		last_hits     = hits;
		last_presents = presents;
		last_time     = time;
	}
}

} // namespace

void Install(uint64_t main_base) {
	const char* enabled = std::getenv("KYTY_TEXTURE_COUNTS");
	if (enabled == nullptr || enabled[0] != '1') {
		return;
	}
	auto* relocate_check = reinterpret_cast<uint8_t*>(main_base + Relocate - RelocateCheck.size());
	auto* relocate       = reinterpret_cast<uint8_t*>(main_base + Relocate);
	auto* bind           = reinterpret_cast<uint8_t*>(main_base + BindTexture);
	auto* material       = reinterpret_cast<uint8_t*>(main_base + MaterialCopy);
	auto* buffer         = reinterpret_cast<uint8_t*>(main_base + BindBuffer);
	auto* commit         = reinterpret_cast<uint8_t*>(main_base + Commit);
	auto* block_copy     = reinterpret_cast<uint8_t*>(main_base + BlockCopy);
	if (std::memcmp(block_copy, BlockMoved.data(), BlockMoved.size()) != 0 ||
	    std::memcmp(reinterpret_cast<uint8_t*>(main_base + BlockCall), BlockCallOp.data(),
	                BlockCallOp.size()) != 0 ||
	    std::memcmp(commit, CommitMoved.data(), CommitMoved.size()) != 0 ||
	    std::memcmp(relocate_check, RelocateCheck.data(), RelocateCheck.size()) != 0 ||
	    std::memcmp(relocate, RelocateMoved.data(), RelocateMoved.size()) != 0 ||
	    std::memcmp(bind, BindMoved.data(), BindMoved.size()) != 0 ||
	    std::memcmp(material, MaterialMoved.data(), MaterialMoved.size()) != 0 ||
	    std::memcmp(buffer, BindMoved.data(), BindMoved.size()) != 0) {
		Log::WriteToConsoleAndLog("Texture binds: not installed (the texture functions differ from "
		                          "GT7 v01.710's)\n");
		return;
	}
	// guest runtime memory, as the game patches' code caves
	const auto page = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    main_base, 0x4000, Common::VirtualMemory::Mode::ExecuteReadWrite, "gt7_texture_counts",
	    false);
	if (page == 0) {
		Log::WriteToConsoleAndLog("Texture binds: not installed (no memory for the stubs)\n");
		return;
	}
	Gt7GuestHook::Emitter r {reinterpret_cast<uint8_t*>(page)};
	const auto relocate_stub = r.Here();
	r.CallHost(reinterpret_cast<const void*>(&OnPackage)); // rdi: the package
	r.Return(RelocateMoved, main_base + Relocate);

	Gt7GuestHook::Emitter b {reinterpret_cast<uint8_t*>(page + 0x200)};
	const auto bind_stub = b.Here();
	b.CallHost(reinterpret_cast<const void*>(&OnBindTexture)); // rdi: the T#
	b.Return(BindMoved, main_base + BindTexture);

	Gt7GuestHook::Emitter c {reinterpret_cast<uint8_t*>(page + 0x400)};
	const auto material_stub = c.Here();
	c.CallHost(reinterpret_cast<const void*>(&OnMaterialBlock)); // edx: size, rcx: source
	c.Return(MaterialMoved, main_base + MaterialCopy);

	Gt7GuestHook::Emitter v {reinterpret_cast<uint8_t*>(page + 0x600)};
	const auto buffer_stub = v.Here();
	v.CallHost(reinterpret_cast<const void*>(&OnBindBuffer)); // rdi: the V#
	v.Return(BindMoved, main_base + BindBuffer);

	Gt7GuestHook::Emitter m {reinterpret_cast<uint8_t*>(page + 0x800)};
	const auto commit_stub = m.Here();
	m.CallHost(reinterpret_cast<const void*>(&OnCommit)); // rdi: the render context
	m.Return(CommitMoved, main_base + Commit);

	// mid-function: the moved instructions first (they set memcpy's arguments), then the host
	// call with them, then on to the memcpy call
	Gt7GuestHook::Emitter k {reinterpret_cast<uint8_t*>(page + 0xa00)};
	const auto block_stub = k.Here();
	k.Append(BlockMoved);
	k.CallHostAligned(reinterpret_cast<const void*>(&OnBlockCopy)); // rdi, rsi, edx
	k.JumpAbsolute(main_base + BlockCall);
	Common::VirtualMemory::FlushInstructionCache(page, 0xc00);

	Gt7GuestHook::PatchEntry(relocate, RelocateMoved.size(), relocate_stub);
	Gt7GuestHook::PatchEntry(bind, BindMoved.size(), bind_stub);
	Gt7GuestHook::PatchEntry(material, MaterialMoved.size(), material_stub);
	Gt7GuestHook::PatchEntry(buffer, BindMoved.size(), buffer_stub);
	Gt7GuestHook::PatchEntry(commit, CommitMoved.size(), commit_stub);
	Gt7GuestHook::PatchEntry(block_copy, BlockMoved.size(), block_stub);
	std::thread(Report).detach();
	Log::WriteToConsoleAndLog("Texture binds: installed on gt7_RelocateTexturePackage, "
	                          "gt7_BindTexture, the material block copies, gt7_BindBuffer and "
	                          "gt7_CommitDrawState\n");
}

} // namespace Loader::Gt7TextureCounts
