#pragma once
#include "common.h"
#include "ndsHeader.h"

/// @brief The arm7's register shadow (ShadowPatchCode7.s): the stores to the
///        sound channels' write-only registers, the timer reloads and the
///        capture registers get stubs that keep the values, so the exit combo's
///        arm7 half can start a loaded state's channels again.
namespace ShadowInstall7
{
    /// @brief Scans the arm7 (still as loaded, autoloads at their initial
    ///        place) for the stores. Patches nothing.
    void Collect(const nds_header_ntr_t* romHeader);

    /// @brief The space Install needs, 0 when there's nothing to do.
    u32 SpaceNeeded();

    /// @brief Puts the code and stubs in the space and patches the sites.
    /// @param space The space, as the loader writes it.
    /// @param runAddress Where the game sees it (32-byte aligned).
    bool Install(void* space, u32 runAddress, u32 size);

    /// @brief Where the game sees the shadow (its 'SHD7' header), after Install.
    u32 RunAddress();
}
