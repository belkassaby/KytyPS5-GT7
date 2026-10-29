#ifndef KYTY_LOADER_GT7TEXTURECOUNTS_H_
#define KYTY_LOADER_GT7TEXTURECOUNTS_H_

#include <cstdint>

// GT7 port, diagnostics (KYTY_TEXTURE_COUNTS=1): how many of GT7's texture bindings come from its
// texture packages (docs/GT7-RENDER-FRONTEND.md). A hook in gt7_RelocateTexturePackage (eboot
// 0x900688100, after its "relocated already" check) records each package's 0x48-byte texture
// records; one on gt7_BindTexture (0x9000f4fc0) looks up the T# it is given. Every 10 s a
// "Texture binds:" line gives, per frame, the binds, the share whose T# is a package record (and
// how many distinct records), and the busiest textures bound from anywhere else, by base address.
// A third hook, on the allocation of a material's constants (0x9000665f0, from gt7_BindMaterial),
// scans the material's constant block for package T#s, and a "Material blocks:" line gives, per
// frame, the blocks, those holding package T#s and how many. A fourth, on gt7_BindBuffer
// (0x901a3ceb0), scans the memory each bound V# describes (most often a material's persistent
// constants; up to 4 KB, committed guest memory only) the same way ("Bound buffers:"). A fifth,
// on gt7_CommitDrawState (0x900016370, every draw), looks in each stage whose user data changed
// for package T#s in the SGPRs and behind SGPR pairs holding a guest pointer (512 bytes), and a
// "Commit scan:" line gives them per frame by stage, with the busiest SGPRs and offsets. Once
// 50,000 package T#s have been seen behind stage 1's SGPRs 2-3 (the texture table pointer), hardware
// write watches (Common::GuestWatch; KYTY_GUEST_WATCH_PER_PC=<n> logs each writer) go on those
// SGPRs of one render context and on the table's first T#, to name the code that writes them
// (KYTY_TEXTURE_WATCH=commit). A sixth hook, in gt7_BindMaterial at its copy of a material's
// constant block into the command buffer (0x900012cea, before the memcpy), scans the blocks
// ("Material blocks (gt7_BindMaterial copy):"); with KYTY_TEXTURE_WATCH=material it keeps the
// first package-T# slot of four blocks and arms value-filtered watches on them, so loading the
// next race names the code that writes material T#s.
// The hooked code is checked against GT7 v01.710's first; on any other nothing is patched.
namespace Loader::Gt7TextureCounts {

// After the main program is loaded and relocated, before it runs.
void Install(uint64_t main_base);

} // namespace Loader::Gt7TextureCounts

#endif // KYTY_LOADER_GT7TEXTURECOUNTS_H_
