// TH07: collect the game's songs (every th07_*.mid in th07.dat, th07_13b.mid
// included, in name order) and init.mid through the engine's Pbg4Archive, and
// build the gm.dls subset blob with MidiBuild.  Platform independent; the PC
// check compiles the same file.
#pragma once

#include "MidiBuild.hpp"

namespace th07midi
{

// Bank rule C (strict LSB), as the TH07 PSP port.
constexpr tdls::MapRule kGameRule = tdls::MAP_C_STRICT_LSB;
// TH07 1.00b: th07_01.mid .. th07_19.mid and th07_13b.mid.
constexpr u32 kGameSongCount = 20;

// datPath is opened through the engine's file layer (relative paths resolve to
// the game directory on the Vita).  dls/dlsSha256 are the whole gm.dls and its
// hash.  On success out.blob is owned by the caller (release with allocator).
bool BuildGameBlob(const char *datPath, const u8 *dls, u32 dlsBytes, const u8 dlsSha256[32],
                   const BuildAllocator &allocator, BuildResult &out, u32 *songCount, BuildProgress progress,
                   void *progressUser);

} // namespace th07midi
