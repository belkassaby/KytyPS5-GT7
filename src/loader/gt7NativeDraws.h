#ifndef KYTY_LOADER_GT7NATIVEDRAWS_H_
#define KYTY_LOADER_GT7NATIVEDRAWS_H_

#include <cstdint>

// GT7 port, native path, milestone 1 (KYTY_GT7_NATIVE=1; docs/GT7-NATIVE-PATH.md): GT7's draws
// carry their engine identity to Kyty's command processor, in stream order. Hooks on
// gt7_BindMaterial (eboot 0x900011800) and gt7_DrawMeshClusters (0x900005fb0) keep, per game
// thread, the material bound last and the mesh drawn; when the game's sceAgcDcbDrawIndex returns
// into gt7_DrawIndex or gt7_DrawMeshClusters, Kyty's Agc (libs/agc.cpp) writes a marker packet
// (an IT_NOP with custom code R_GT7_DRAW) with them just before the draw packet. The command
// processor takes the marker for the next draw; for now it only checks it: whether the pixel
// stage's SGPRs 2-3 (its texture table) are the material's block, and a "Native draws:" line
// every 10 s gives the counts. Nothing is drawn differently. The hooked code is checked against
// GT7 v01.710's first; on any other nothing is patched and no marker is written.
namespace Loader::Gt7NativeDraws {

void Install(uint64_t main_base);

[[nodiscard]] bool Enabled();

struct Identity {
	uint64_t set      = 0; // the material set (gt7_BindMaterial's first argument)
	uint64_t material = 0; // its GT7Material
	uint64_t block    = 0; // the material's constant block, when bound
	uint64_t shaders  = 0; // the shader set (fourth argument)
	uint64_t mesh     = 0; // gt7_DrawMeshClusters's first argument, or 0
	uint32_t sequence = 0; // this thread's marker count
};

// The marker: header, then the identity's fields as dwords.
constexpr uint32_t MarkerDwords = 1 + 5 * 2 + 1;

// Agc (a game thread, in sceAgcDcbDrawIndex): the identity to mark this draw with, if the call
// returns into GT7's engine draws.
[[nodiscard]] bool ForDraw(uint64_t guest_return_address, Identity* identity);
void               WriteMarker(uint32_t* packet, const Identity& identity);

// Command processor (the GPU thread): a marker's payload, then the next indexed draw with the
// pixel stage's user-data SGPRs.
void OnMarker(const uint32_t* payload, uint32_t dwords);
void OnDraw(const uint32_t* ps_user_sgprs, uint32_t count);
// After the renderer's DrawIndex, with the time it took.
void OnDrawDone(uint64_t renderer_ns);

// Milestone 3, measured first (the renderer, GPU thread): a marked draw's programs (a signature of
// their ids) and pipeline, after the pipeline lookup. A "Native templates:" line every 10 s gives
// how often a marked draw's (material, mesh, programs, pipeline) was seen the frame before, and
// the renderer's time for marked and other draws: what draw templates could take over.
void NoteTemplate(uint64_t programs, uint64_t pipeline);

// Milestone 2 (the renderer, GPU thread, while a draw executes): the draw's material when its
// pixel stage's texture table is that material's block, else 0. With it the pixel stage's texture
// and sampler bindings are kept per (material, program) and reused while their descriptors and
// images are unchanged (RenderExecutor::PrepareBindings; KYTY_GT7_NATIVE_BINDINGS=0 turns it off,
// still timing the same draws, for an A/B; KYTY_GT7_NATIVE_VERIFY=1 resolves every reused
// binding again and counts the ones that differ).
[[nodiscard]] uint64_t CurrentPixelMaterial();
[[nodiscard]] bool     BindingsEnabled();
[[nodiscard]] bool     BindingsVerify();
void                   AddPixelBindings(bool hit, bool uncacheable, uint64_t nanoseconds);
void                   AddBindingMismatch();

} // namespace Loader::Gt7NativeDraws

#endif // KYTY_LOADER_GT7NATIVEDRAWS_H_
