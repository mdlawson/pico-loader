#pragma once
#include "common.h"

class IAutoloadAdjuster;

/// @brief The register shadow without traps: the loader finds the arm9's stores
///        to the display registers that can't be read back and gives each a
///        stub that also keeps the value (ShadowPatchCode.s), for a state load
///        to write back.
namespace ShadowInstall
{
    /// @brief Scans the decompressed arm9 (its autoloads still at their initial
    ///        place) for the stores and for the sdk's region 2 setup. Patches nothing.
    void Collect(u32 arm9Start, u32 arm9Size, const IAutoloadAdjuster* autoload);

    /// @brief The space Install needs, 0 when there's nothing to do.
    u32 SpaceNeeded();

    /// @brief Puts the code and stubs in the space and patches the sites.
    /// @param space The space, as the loader writes it.
    /// @param runAddress Where the game sees it.
    /// @param size The space it got (may be less than SpaceNeeded: sites left out).
    bool Install(void* space, u32 runAddress, u32 size);
}
