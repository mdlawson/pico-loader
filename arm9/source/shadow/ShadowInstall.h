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

    /// @brief The game's MIi_UncompressBackward, for the overlays.
    void SetUncompress(const u32* uncompress);

    /// @brief Where the overlay hook (ShadowOverlayPatch) finds the shadow's
    ///        routine for a loaded overlay; Install puts it in.
    void SetOverlayHookTarget(u32* target);

    /// @brief Whether ScanOverlay is of use (the arm7 then reads the overlays).
    bool WantsOverlays();

    /// @brief Scans an overlay the arm7 read into place (still compressed, if
    ///        it is), then clears its memory again.
    /// @param entry Its overlay table entry.
    void ScanOverlay(const u32* entry, u32 fileSize);

    /// @brief The space Install needs, 0 when there's nothing to do.
    u32 SpaceNeeded();

    /// @brief Puts the code and stubs in the space and patches the sites.
    /// @param space The space, as the loader writes it.
    /// @param runAddress Where the game sees it.
    /// @param size The space it got (may be less than SpaceNeeded: sites left out).
    bool Install(void* space, u32 runAddress, u32 size);

    /// @brief After Install: the routine the exit combo's vblank hook is to
    ///        call, for overlays no FS_StartOverlay hook catches (0: none).
    u32 VblankRoutine();
}
