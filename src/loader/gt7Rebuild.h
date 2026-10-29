#ifndef KYTY_LOADER_GT7REBUILD_H_
#define KYTY_LOADER_GT7REBUILD_H_

#include <cstdint>

// GT7 port, research (KYTY_GT7_REBUILD=<dir>): runs GT7 functions rebuilt in C from the Ghidra
// decompile (GRANTURISMO7\rebuild, tools/build.py). <dir>/rebuild.bin is mapped at the address
// <dir>/rebuild.map gives (its first line: "base <hex> size <hex>"), and each function the map
// lists ("<original entry> <compiled entry> <name>") gets a jump from its original entry to the
// compiled one; everything else runs as the original code. Only with the eboot at 0x900000000
// (the address the rebuild links against).
namespace Loader::Gt7Rebuild {

void Install(uint64_t main_base);

} // namespace Loader::Gt7Rebuild

#endif // KYTY_LOADER_GT7REBUILD_H_
