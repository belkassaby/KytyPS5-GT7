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

} // namespace Loader::Gt7NativeDraws

#endif // KYTY_LOADER_GT7NATIVEDRAWS_H_
