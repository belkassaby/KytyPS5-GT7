#include "loader/gt7Rebuild.h"

#include "common/logging/log.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "loader/gt7GuestHook.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>

namespace Loader::Gt7Rebuild {

namespace {

constexpr uint64_t LinkedBase = 0x900000000; // tools/build.py links against the eboot here
constexpr uint64_t PageSize   = 0x4000;

void Fail(const std::string& why) {
	Log::WriteToConsoleAndLog(fmt::format("GT7 rebuild: not installed ({})\n", why));
}

} // namespace

void Install(uint64_t main_base) {
	const char* dir = std::getenv("KYTY_GT7_REBUILD");
	if (dir == nullptr || dir[0] == '\0') {
		return;
	}
	if (main_base != LinkedBase) {
		Fail(fmt::format("the eboot is at 0x{:x}, the rebuild is linked for 0x{:x}", main_base,
		                 LinkedBase));
		return;
	}
	const std::filesystem::path out(dir);
	std::ifstream map_file(out / "rebuild.map");
	std::ifstream bin_file(out / "rebuild.bin", std::ios::binary);
	if (!map_file || !bin_file) {
		Fail(fmt::format("no rebuild.map or rebuild.bin in {}", dir));
		return;
	}
	std::string header;
	std::getline(map_file, header);
	uint64_t base = 0;
	uint64_t size = 0;
	{
		std::istringstream line(header);
		std::string        word_base;
		std::string        word_size;
		line >> word_base >> std::hex >> base >> word_size >> size;
		if (word_base != "base" || word_size != "size" || base == 0 || size == 0) {
			Fail("rebuild.map has no \"base <hex> size <hex>\" line");
			return;
		}
	}
	std::vector<char> code((std::istreambuf_iterator<char>(bin_file)), std::istreambuf_iterator<char>());
	if (code.size() > size) {
		Fail("rebuild.bin is larger than the map says");
		return;
	}
	const auto mapped = (size + PageSize - 1) & ~(PageSize - 1);
	const auto page   = Libs::LibKernel::Memory::AllocateRuntimeMemory(
        base, mapped, Common::VirtualMemory::Mode::ExecuteReadWrite, "gt7_rebuild", true);
	if (page != base) {
		Fail(fmt::format("0x{:x} is not free for the rebuilt code", base));
		return;
	}
	std::memset(reinterpret_cast<void*>(base), 0, mapped);
	std::memcpy(reinterpret_cast<void*>(base), code.data(), code.size());
	Common::VirtualMemory::FlushInstructionCache(base, mapped);

	uint32_t    patched = 0;
	std::string entry;
	while (std::getline(map_file, entry)) {
		std::istringstream line(entry);
		uint64_t           original = 0;
		uint64_t           compiled = 0;
		std::string        name;
		line >> std::hex >> original >> compiled >> name;
		if (original < main_base || compiled < base || compiled >= base + size) {
			continue;
		}
		Gt7GuestHook::PatchEntry(reinterpret_cast<uint8_t*>(original), 14, compiled);
		patched++;
		if (patched <= 16) {
			Log::WriteToConsoleAndLog(
			    fmt::format("GT7 rebuild: 0x{:x} {} -> 0x{:x}\n", original, name, compiled));
		}
	}
	Log::WriteToConsoleAndLog(fmt::format("GT7 rebuild: {} functions run from C ({} bytes at 0x{:x})\n",
	                                      patched, code.size(), base));
}

} // namespace Loader::Gt7Rebuild
