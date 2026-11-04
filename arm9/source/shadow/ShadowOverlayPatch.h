#pragma once
#include "patches/arm9/OverlayPatches/OverlayPatch.h"

/// @brief The register shadow's part of the overlay hook: once the loader has
///        put in the shadow (ShadowInstall), each overlay that loads gets its
///        stores patched to the stubs the loader made for it at boot.
class ShadowOverlayPatch : public OverlayPatch
{
public:
    const void* InsertPatch(PatchContext& patchContext) override;
};
