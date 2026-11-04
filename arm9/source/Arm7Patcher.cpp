#include "common.h"
#include <libtwl/mem/memNtrWram.h>
#include "ModuleParamsLocator.h"
#include "SdkVersion.h"
#include "sharedMemory.h"
#include "gameCode.h"
#include "cache.h"
#include "errorDisplay/ErrorDisplay.h"
#include "patches/PatchCollection.h"
#include "patches/PatchContext.h"
#include "patches/platform/LoaderPlatform.h"
#include "patches/arm7/sdk2to4/CardiTaskThreadPatch.h"
#include "patches/arm7/sdk5/BannerSavePatch.h"
#include "patches/arm7/sdk5/CardiDoTaskFromArm9Patch.h"
#include "patches/arm7/OsGetInitArenaLoPatch.h"
#include "patches/arm7/DisableArm7WramClearPatch.h"
#include "patches/arm7/sdk5/Sdk5DsiSdCardRedirectPatch.h"
#include "patches/arm7/PokemonDownloaderArm7Patch.h"
#include "patches/arm7/cheats/CheatEnginePatch.h"
#include "patches/arm7/ExitComboArm7Patch.h"
#include "patches/arm7/DsiBiosPitchTablePatch.h"
#include "patches/DsiBiosLz77SwiPatch.h"
#include "Arm7Patcher.h"

static u32 correctAddress(u32 address, bool runInDSiMode)
{
    // A DSi rom that falls back to DS mode needs the 0x023... form as well,
    // the game reads its arena in 4MB mode.
    if (gIsDsiMode && !runInDSiMode)
    {
        return address & ~0xC00000; // 0x023...
    }
    else
    {
        return address | 0x800000; // make sure it ends up in the right place while in 16MB mode
    }
}

void* Arm7Patcher::ApplyPatches(const LoaderPlatform* loaderPlatform, u32 cheatsLength, void*& cheatsPtr,
    char*& bannerSavePathPtr, bool exitComboArm9Installed, bool& exitComboArm7Installed, bool runInDSiMode,
    bool keepDsiBios, void*& censusSpace, u32& censusRunAddress, u32& censusSize) const
{
    cheatsPtr = nullptr;
    exitComboArm7Installed = false;
    auto romHeader = (const nds_header_ntr_t*)TWL_SHARED_MEMORY->ntrSharedMem.romHeader;
    auto twlRomHeader = (const nds_header_twl_t*)TWL_SHARED_MEMORY->twlRomHeader;
    ModuleParamsLocator moduleParamsLocator;
    auto moduleParams = moduleParamsLocator.FindModuleParams(romHeader);
    SdkVersion sdkVersion = moduleParams ? moduleParams->sdkVersion : 0u;
    if (!moduleParams && romHeader->gameCode == GAMECODE("AS2E"))
    {
        // Spider-Man 2 (USA) is probably the only game without module params
        sdkVersion = 0x02004F50;
    }
    std::unique_ptr<IAutoloadAdjuster> arm7Autoload; // TODO unused
    std::unique_ptr<IAutoloadAdjuster> arm7iAutoload;
    if (runInDSiMode)
    {
        auto arm7iModuleParams = (const module_params_twl_t*)(twlRomHeader->arm7LoadAddress + twlRomHeader->arm7iModuleParamsAddress);
        arm7iAutoload = std::make_unique<AutoloadAdjuster<autoload_list_entry_sdk5_t>>(
            (autoload_list_entry_sdk5_t*)arm7iModuleParams->autoloadListStart,
            (autoload_list_entry_sdk5_t*)arm7iModuleParams->autoloadListEnd,
            arm7iModuleParams->autoloadStart);
    }
    PatchCollection patchCollection;
    LOG_DEBUG("Arm7 region: 0x%x - 0x%x\n", romHeader->arm7LoadAddress, romHeader->arm7LoadAddress + romHeader->arm7Size);
    PatchContext patchContext
    {
        (void*)romHeader->arm7LoadAddress,
        romHeader->arm7Size,
        std::move(arm7Autoload),
        (romHeader->SupportsDsiMode()) ? (void*)twlRomHeader->arm7iLoadAddress : nullptr,
        (romHeader->SupportsDsiMode()) ? twlRomHeader->arm7iSize : 0,
        std::move(arm7iAutoload),
        sdkVersion,
        romHeader->gameCode,
        romHeader->softwareVersion,
        loaderPlatform
    };
    void* patchSpaceStart = nullptr;
    if (sdkVersion != 0)
    {
        mem_setNtrWramMapping(MEM_NTR_WRAM_ARM9, MEM_NTR_WRAM_ARM9);
        auto arm7ArenaPatch = new OsGetInitArenaLoPatch();
        patchCollection.AddPatch(arm7ArenaPatch);

        if (!arm7ArenaPatch->FindPatchTarget(patchContext))
        {
            ErrorDisplay().PrintError("Failed to apply arm7 patches.");
        }

        const u32 arm7PatchSpaceSize = ARM7_PATCH_SPACE_SIZE;
        void* privateWramHeapStart = arm7ArenaPatch->GetArm7PrivateWramArenaLo();
        u32 mainMemoryArenaLo = (u32)arm7ArenaPatch->GetMainMemoryArenaLo();
        if (0x0380F780 - (u32)privateWramHeapStart - 0x2100 >= arm7PatchSpaceSize)
        {
            patchSpaceStart = privateWramHeapStart;
            arm7ArenaPatch->SetArm7PrivateWramArenaLo((u8*)patchSpaceStart + arm7PatchSpaceSize);
        }
        else
        {
            LOG_DEBUG("Arm 7 patches placed in main memory\n");
            patchSpaceStart = (void*)correctAddress(mainMemoryArenaLo, runInDSiMode);
            mainMemoryArenaLo += arm7PatchSpaceSize;
        }

        patchContext.GetPatchHeap().AddFreeSpace(patchSpaceStart, arm7PatchSpaceSize);

        if (cheatsLength > 0)
        {
            void* cheats = (void*)correctAddress(mainMemoryArenaLo, runInDSiMode);
            LOG_DEBUG("Cheats placed at 0x%p\n", cheats);
            cheatsPtr = cheats;
            patchCollection.AddPatch(new CheatEnginePatch(cheats));
            mainMemoryArenaLo += cheatsLength;
        }

        // The in-game exit needs its arm7 half as well, once the arm9 half is in
        // and the arm7 dispatcher was found. It only takes patch heap, so the
        // save buffer and banner path below stay where they are without it.
        ExitComboArm7Patch* exitComboArm7Patch = nullptr;
        if (exitComboArm9Installed)
        {
            exitComboArm7Patch = new ExitComboArm7Patch();
            exitComboArm7Patch->FindPatchTarget(patchContext);
            if (!exitComboArm7Patch->HasTarget())
            {
                exitComboArm7Patch = nullptr;
            }
        }

        if (romHeader->unitCode == 0) // seems only present on NITRO, not on HYBRID or LIMITED
        {
            patchCollection.AddPatch(new DisableArm7WramClearPatch());
        }

        if (sdkVersion.IsTwlSdk())
        {
            if (runInDSiMode && (twlRomHeader->HasNandAccess() || twlRomHeader->HasSdAccess()))
            {
                patchCollection.AddPatch(new Sdk5DsiSdCardRedirectPatch());
            }

            if (twlRomHeader->IsDsiWare())
            {
                if (twlRomHeader->twlFlags2 & NDS_HEADER_TWL_FLAGS_2_HAS_BANNER_SAVE)
                {
                    char* bannerSavePath = (char*)correctAddress(mainMemoryArenaLo, runInDSiMode);
                    LOG_DEBUG("Banner save path placed at 0x%p\n", bannerSavePath);
                    bannerSavePathPtr = bannerSavePath;
                    patchCollection.AddPatch(new BannerSavePatch(bannerSavePath));
                    mainMemoryArenaLo += 64;
                }
            }
            else
            {
                void* saveTmpBuffer = (void*)correctAddress(mainMemoryArenaLo, runInDSiMode);
                mainMemoryArenaLo += 512;
                patchCollection.AddPatch(new CardiDoTaskFromArm9Patch(saveTmpBuffer));
            }
        }
        else
        {
            void* saveTmpBuffer = (void*)correctAddress(mainMemoryArenaLo, runInDSiMode);
            mainMemoryArenaLo += 512;
            patchCollection.AddPatch(new CardiTaskThreadPatch(saveTmpBuffer));
        }

        bool isPokemonDownloader = *(vu32*)0x02FFF00C == GAMECODE("ADAJ") &&
            romHeader->arm9LoadAddress == 0x02004000 &&
            romHeader->arm9EntryAddress == 0x02004800;
        if (isPokemonDownloader)
        {
            patchCollection.AddPatch(new PokemonDownloaderArm7Patch());
        }

        if (keepDsiBios)
        {
            // The bios of the DSi differs from the one of the DS in two calls
            // the sdk makes. Ahead of the exit combo on purpose: the game keeps
            // the dsi bios even if the combo's arm7 half does not fit after all.
            patchCollection.AddPatch(new DsiBiosLz77SwiPatch(true));
            patchCollection.AddPatch(new DsiBiosPitchTablePatch());
        }

        // Added last on purpose: this is an optional feature that must never take
        // patch heap a mandatory patch needs. Because ApplyPatch runs in insertion
        // order, every mandatory patch above allocates first and the exit combo
        // only claims what is left (and gives up when nothing fits). The arm9
        // half already left out DSi mode, DSiWare and the Pokemon downloader.
        if (exitComboArm7Patch != nullptr)
        {
            patchCollection.AddPatch(exitComboArm7Patch);
        }

        // The register-write census, for the exit combo's hook to run on the arm9,
        // from the start of the arm7's main memory arena. That arena is usually
        // in the arm7's own corner (0x027E0000 and up, below the shared page);
        // the arm9 runs the census through the 4 MB view of the same memory,
        // where the census gate turns the sdk's region 2 guard into the dsi
        // bios's exception stack page, leaving that corner to region 1.
        censusSpace = nullptr;
        censusRunAddress = 0;
#ifdef RTS_CENSUS
        if (exitComboArm7Patch != nullptr && !sdkVersion.IsTwlSdk())
#else
        if (false)
#endif
        {
            u32 start = (mainMemoryArenaLo + 31) & ~31u;
            u32 offset = start & 0x3FFFFF;
            // a big pool when the arm7 can spare it (pokemon's arena is small)
            censusSize = offset + CENSUS_SPACE_SIZE + CENSUS_ARM7_SPARE <= 0x3FF000 ? CENSUS_SPACE_SIZE : CENSUS_SPACE_MIN;
            if (start >= 0x02000000 && start < 0x02800000 && offset + censusSize <= 0x3FF000)
            {
                // written through the loader's view of main memory, run at
                // the game's: the arm9's protection unit only maps the 4 MB
                censusSpace = (void*)correctAddress(start, romHeader);
                censusRunAddress = 0x02000000 + offset;
                mainMemoryArenaLo = start + censusSize;
                LOG_DEBUG("Census space at 0x%x, 0x%x bytes, written at 0x%p\n", censusRunAddress, censusSize, censusSpace);
            }
            else
            {
                LOG_DEBUG("No census space: arm7 main memory arena at 0x%x\n", mainMemoryArenaLo);
            }
        }

        if (exitComboArm7Patch != nullptr)
        {
            exitComboArm7Patch->SetPatchSpace((u32)patchSpaceStart);
        }

        arm7ArenaPatch->SetMainMemoryArenaLo((void*)mainMemoryArenaLo);

        // The arm7 patcher uses the fact that the ntr wram is mirrored over the entire 03 region.
        // Since the reserved patch space is smaller than the ntr wram, it will never overlap.
        // As a result, the patcher can use arm7 addresses for placing patches.
        // The arm7 will copy the patch data to the actual arm7 location afterwards.

        // If in DSi mode and the rom is a DSi rom, temporarily disable twl wram to let ntr wram cover the entire 03 region.
        u32 mbk6 = 0;
        u32 mbk7 = 0;
        u32 mbk8 = 0;
        if (runInDSiMode)
        {
            mbk6 = REG_MBK6;
            mbk7 = REG_MBK7;
            mbk8 = REG_MBK8;
            REG_MBK6 = 0;
            REG_MBK7 = 0;
            REG_MBK8 = 0;
        }

        if (!patchCollection.TryPerformPatches(patchContext))
        {
            ErrorDisplay().PrintError("Failed to apply arm7 patches.");
        }
        dc_flushAll();
        dc_drainWriteBuffer();
        ic_invalidateAll();

        // the arm7 half may not have fit after all
        exitComboArm7Installed = exitComboArm7Patch != nullptr && exitComboArm7Patch->IsInstalled();

        // If in DSi mode and the rom is a DSi rom, restore twl wram.
        if (runInDSiMode)
        {
            REG_MBK6 = mbk6;
            REG_MBK7 = mbk7;
            REG_MBK8 = mbk8;
        }

        mem_setNtrWramMapping(MEM_NTR_WRAM_ARM7, MEM_NTR_WRAM_ARM7);
    }
    else
    {
        LOG_DEBUG("Module params not found!\n");
    }
    return patchSpaceStart;
}
