#pragma once
#include "../LoaderPlatform.h"
#include "MelonDSReadSdPatchCode.h"
#include "MelonDSWriteSdPatchCode.h"
#include "melonDSDldi_bin.h"

/// @brief Implementation of LoaderPlatform for MelonDS
class MelonDSLoaderPlatform : public LoaderPlatform
{
public:
    const IReadSectorsPatchCode* CreateSdReadPatchCode(
        PatchCodeCollection& patchCodeCollection, PatchHeap& patchHeap) const override
    {
        return patchCodeCollection.GetOrAddSharedPatchCode([&]
        {
            return new MelonDSReadSdPatchCode(patchHeap);
        });
    }

    bool HasSdReadPatchCode(const PatchCodeCollection& patchCodeCollection) const override
    {
        return patchCodeCollection.HasSharedPatchCode<MelonDSReadSdPatchCode>();
    }

    const IWriteSectorsPatchCode* CreateSdWritePatchCode(
        PatchCodeCollection& patchCodeCollection, PatchHeap& patchHeap) const override
    {
        return patchCodeCollection.GetOrAddSharedPatchCode([&]
        {
            return new MelonDSWriteSdPatchCode(patchHeap);
        });
    }

    LoaderPlatformType GetPlatformType() const override { return LoaderPlatformType::Slot1; }

    /// @note melonDLDI from melonDS (src/NDSCart/melonDLDI.h), the driver melonDS
    ///       patches into homebrew with an sd card; it speaks the same card
    ///       commands as the patch code above.
    const void* GetBuiltInDldiDriver(u32& size) const override
    {
        size = melonDSDldi_bin_size;
        return melonDSDldi_bin;
    }
};
