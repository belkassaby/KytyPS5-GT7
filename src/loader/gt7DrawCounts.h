#ifndef KYTY_LOADER_GT7DRAWCOUNTS_H_
#define KYTY_LOADER_GT7DRAWCOUNTS_H_

#include <cstdint>

// GT7 port, diagnostics (KYTY_DRAW_COUNTS=1): how many draws GT7's cluster-culled mesh draw makes.
// gt7_DrawMeshClusters (eboot 0x900005fb0) culls a mesh's index clusters and issues one
// gt7_DrawIndex (0x9000a1400) per run of visible clusters (docs/GT7-RENDER-FRONTEND.md). Both
// entries jump to a counting stub that then runs the moved prologue and jumps back. Every 10 s a
// "Draw counts:" line gives, per frame, the mesh draws, the clusters they were given, the
// gt7_DrawIndex calls they made and all gt7_DrawIndex calls. The entries are checked against the
// expected bytes first (PPSA01317 v01.710); on any other code nothing is patched.
namespace Loader::Gt7DrawCounts {

// After the main program is loaded and relocated, before it runs.
void Install(uint64_t main_base);

} // namespace Loader::Gt7DrawCounts

#endif // KYTY_LOADER_GT7DRAWCOUNTS_H_
