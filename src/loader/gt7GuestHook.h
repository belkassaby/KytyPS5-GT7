#ifndef KYTY_LOADER_GT7GUESTHOOK_H_
#define KYTY_LOADER_GT7GUESTHOOK_H_

#include "common/virtualMemory.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>

// GT7 port, diagnostics: entry hooks on guest functions (gt7DrawCounts, gt7TextureCounts). A
// hooked entry jumps to a stub that does its work, runs the moved first instructions (they must
// not be RIP-relative or branch) and jumps back after them.
namespace Loader::Gt7GuestHook {

struct Emitter {
	uint8_t* code;
	size_t   pos = 0;

	[[nodiscard]] uint64_t Here() const { return reinterpret_cast<uint64_t>(code + pos); }
	void Bytes(std::initializer_list<uint8_t> bytes) {
		for (auto b: bytes) {
			code[pos++] = b;
		}
	}
	void Append(std::span<const uint8_t> bytes) {
		std::memcpy(code + pos, bytes.data(), bytes.size());
		pos += bytes.size();
	}
	void U32(uint32_t v) {
		std::memcpy(code + pos, &v, 4);
		pos += 4;
	}
	void U64(uint64_t v) {
		std::memcpy(code + pos, &v, 8);
		pos += 8;
	}
	// opcode bytes then a disp32 to `target` from the end of the instruction
	void RipRelative(std::initializer_list<uint8_t> opcode, const void* target) {
		Bytes(opcode);
		const auto end = Here() + 4;
		U32(static_cast<uint32_t>(reinterpret_cast<int64_t>(target) - static_cast<int64_t>(end)));
	}
	void JumpAbsolute(uint64_t target) {
		Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip+0]
		U64(target);
	}
	// Calls a KYTY_SYSV_ABI host function with the guest's argument registers as they are, keeping
	// every caller-saved register and xmm0-7. Only at a function entry (rsp 8 mod 16).
	void CallHost(const void* function) {
		Bytes({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});
		Bytes({0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00}); // sub rsp, 0x80
		for (uint8_t i = 0; i < 8; i++) {
			Bytes({0xf3, 0x0f, 0x7f, static_cast<uint8_t>(0x44 | (i << 3u)), 0x24,
			       static_cast<uint8_t>(i * 16)}); // movdqu [rsp+16i], xmm_i
		}
		Bytes({0x48, 0xb8}); // mov rax, imm64
		U64(reinterpret_cast<uint64_t>(function));
		Bytes({0xff, 0xd0}); // call rax
		for (uint8_t i = 0; i < 8; i++) {
			Bytes({0xf3, 0x0f, 0x6f, static_cast<uint8_t>(0x44 | (i << 3u)), 0x24,
			       static_cast<uint8_t>(i * 16)}); // movdqu xmm_i, [rsp+16i]
		}
		Bytes({0x48, 0x81, 0xc4, 0x80, 0x00, 0x00, 0x00}); // add rsp, 0x80
		Bytes({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58});
	}
	// CallHost from anywhere in a function body: aligns the stack itself (rbx keeps the old rsp;
	// the flags are changed, so only where they are not live).
	void CallHostAligned(const void* function) {
		Bytes({0x53});                         // push rbx
		Bytes({0x48, 0x89, 0xe3});             // mov rbx, rsp
		Bytes({0x48, 0x83, 0xe4, 0xf0});       // and rsp, -16
		Bytes({0x48, 0x83, 0xec, 0x08});       // sub rsp, 8 (as at a function entry)
		CallHost(function);
		Bytes({0x48, 0x89, 0xdc});             // mov rsp, rbx
		Bytes({0x5b});                         // pop rbx
	}
	// The end of a stub: the moved instructions, then back to the hooked code after them.
	void Return(std::span<const uint8_t> moved, uint64_t entry) {
		Append(moved);
		JumpAbsolute(entry + moved.size());
	}
};

// Points `entry` (whose first `moved` bytes the stub runs) at `stub`.
inline void PatchEntry(uint8_t* entry, size_t moved, uint64_t stub) {
	Emitter e {entry};
	e.JumpAbsolute(stub);
	std::memset(entry + e.pos, 0xcc, moved - e.pos);
	Common::VirtualMemory::FlushInstructionCache(reinterpret_cast<uint64_t>(entry), moved);
}

} // namespace Loader::Gt7GuestHook

#endif // KYTY_LOADER_GT7GUESTHOOK_H_
