#ifndef EMULATOR_SRC_COMMON_FILEREADLOG_H_
#define EMULATOR_SRC_COMMON_FILEREADLOG_H_

#include <cstdint>
#include <string>

// GT7 port, render mapping (diagnostics, on with KYTY_MAP_TEXTURES=1): the guest's file reads, by
// the guest memory they filled, so the texture search can tell which file and offset a T# it found
// came from (and whether the bytes there are the file's own or were changed after the read).
namespace Common::FileReadLog {

struct Read {
	uint64_t    destination = 0;
	uint64_t    size        = 0;
	uint64_t    file_offset = 0;
	std::string path;
};

[[nodiscard]] bool Enabled();
// A read of `size` bytes of `path` at `file_offset` into guest memory at `destination`.
void Record(uint64_t destination, uint64_t size, const std::string& path, uint64_t file_offset);
// The latest recorded read that filled `address`; false when none did.
[[nodiscard]] bool Find(uint64_t address, Read* read);

} // namespace Common::FileReadLog

#endif // EMULATOR_SRC_COMMON_FILEREADLOG_H_
