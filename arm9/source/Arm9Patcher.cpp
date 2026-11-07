#include "common.h"
#include "ModuleParamsLocator.h"
#include "AutoloadAdjuster.h"
#include "SdkVersion.h"
#include "patches/PatchCollection.h"
#include "patches/PatchContext.h"
#include "sharedMemory.h"
#include "patches/arm9/sdk2to4/CardiReadCardPatch.h"
#include "patches/arm9/sdk2to4/CardiTryReadCardDmaPatch.h"
#include "patches/arm9/sdk5/CardiIsRomDmaAvailablePatch.h"
#include "patches/arm9/sdk5/CardiReadCardWithHashInternalAsyncPatch.h"
#include "patches/arm9/sdk5/CardiReadRomWithCpuPatch.h"
#include "patches/arm9/CardiReadRomIdCorePatch.h"
#include "patches/arm9/OSResetSystemPatch.h"
#include "patches/arm9/ExitComboPatch.h"
#include "patches/DsiBiosLz77SwiPatch.h"
#include "patches/arm9/PokemonDownloaderArm9Patch.h"
#include "patches/arm9/DSProtectArm9Patch.h"
#include "patches/arm9/LastWindowCrcPatch.h"
#include "patches/arm9/NandSave/FaceTrainingNandSavePatch.h"
#include "patches/arm9/NandSave/JamWithTheBandNandSavePatch.h"
#include "patches/arm9/NandSave/NintendoDSGuideNandSavePatch.h"
#include "patches/arm9/NandSave/WarioWareDiyNandSavePatch.h"
#include "patches/arm9/OverlayPatches/FsStartOverlayHookPatch.h"
#include "patches/arm9/OverlayPatches/DSProtectPatches/DSProtectOverlayPatch.h"
#include "patches/arm9/OverlayPatches/DSProtectPatches/DSProtectPuyoPuyo7Patch.h"
#include "patches/arm9/OverlayPatches/PokemonIr/PokemonIrApPatch.h"
#include "patches/arm9/OverlayPatches/KirbySuperStarUltra/KirbySuperStarUltraPatch.h"
#include "patches/arm9/OverlayPatches/RabbidsGoHome/RabbidsGoHomePatch.h"
#include "patches/arm9/OverlayPatches/GoldenSunDarkDawn/GoldenSunDarkDawnOverlayHookPatch.h"
#include "SecureSysCallsUnusedSpaceLocator.h"
#include "fastSearch.h"
#include "gameCode.h"
#include "cache.h"
#include "ApList.h"
#include "patches/platform/LoaderPlatform.h"
#include "errorDisplay/ErrorDisplay.h"
#include "../../include/picoLoader7.h"
#include "Arm9Patcher.h"
#include "shadow/ShadowInstall.h"
#include "shadow/ShadowOverlayPatch.h"

#define PARENT_SECTION_START    0x02001000
#define PARENT_SECTION_END      0x02003000

#define REQUIRED_PATCH_HEAP_SPACE   0x500

typedef void (*uncompress_func_t)(void* compressedEnd);

static const u32 sMiiUncompressBackwardPatternOld[] = { 0xE3500000, 0x0A000025, 0xE92D00F0, 0xE9100006 };
static const u32 sMiiUncompressBackwardPatternOld2[] = { 0xE3500000, 0x0A00002B, 0xE92D00F0, 0xE9100006 };
static const u32 sMiiUncompressBackwardPattern[] = { 0xE3500000, 0x0A000027, 0xE92D00F0, 0xE9100006 };
static const u32 sMiiUncompressBackwardPatternHybrid[] = { 0xE3500000, 0x0A000029, 0xE92D01F0, 0xE9100006 };

/// @brief The games AddGamePatches gives a NAND save. They save through their own
///        routines on the arm9, which the in-game exit cannot tell apart from play,
///        so it could cut one of their saves in half.
static bool hasNandSave(u32 gameCode)
{
    switch (gameCode)
    {
        case GAMECODE("UORE"):
        case GAMECODE("UORP"):
        case GAMECODE("UORJ"):
        case GAMECODE("UXBP"):
        case GAMECODE("USKV"):
        case GAMECODE("UGDA"):
            return true;
        default:
            return false;
    }
}

#ifdef RTS_AUTOPILOT
#include <string.h>
#include "sections.h"

/// @brief The autopilot build: every literal pool word 0x04000130 (KEYINPUT) that a
///        pc-relative load reads, arm or thumb, in the decompressed arm9 (its
///        autoloads still in place), goes to keys instead, so the game reads the
///        autopilot's keys. (A KEYCNT access through the same base lands on the
///        word's other half.) Overlays keep theirs.
static void redirectKeyInput(u32 start, u32 size, u32 keys)
{
    u32 end = start + (size & ~3u);
    u32 count = 0;
    for (u32 a = (start + 3) & ~3u; a < end; a += 4)
    {
        if (*(const u32*)a != 0x04000130)
            continue;
        bool loaded = false;
        // ldr rX, [pc, #+imm] (arm, up to 4 KB before) / ldr rX, [pc, #imm] (thumb, up to 1 KB)
        for (u32 at = a >= start + 0x1000 ? a - 0x1000 : start; at < a && !loaded; at += 2)
        {
            if ((at & 3) == 0)
            {
                u32 op = *(const u32*)at;
                if ((op & 0x0F7F0000) == 0x051F0000)
                {
                    u32 imm = op & 0xFFF;
                    loaded = (op & (1 << 23) ? at + 8 + imm : at + 8 - imm) == a;
                }
            }
            u16 h = *(const u16*)at;
            if (!loaded && (h & 0xF800) == 0x4800 && a - at <= 0x400)
                loaded = ((at + 4) & ~3u) + (h & 0xFF) * 4 == a;
        }
        if (!loaded)
            continue;
        *(u32*)a = keys;
        count++;
    }
    LOG_DEBUG("Autopilot: %u KEYINPUT literals to 0x%x\n", count, keys);
}

DEFINE_SECTION_SYMBOLS(autopilothalt);
extern "C" void autopilothalt_entry(void);
extern "C" u32 autopilothalt_return;
extern "C" u32 autopilothalt_idle;

/// @brief The autopilot build's timing: every "wait for interrupt" (mcr p15, 0, rX,
///        c7, c0, 4) in the decompressed arm9 and its autoloads branches to a copy of
///        autopilothalt (AutopilotHaltCode.s), which adds the scanlines halted to idle.
static void installHaltTiming(PatchContext& patchContext, u32 start, u32 size, u32 idle)
{
    u32 end = start + (size & ~3u);
    u32 count = 0, failed = 0;
    auto autoload = patchContext.GetAutoloadAdjuster();
    for (u32 a = (start + 3) & ~3u; a < end; a += 4)
    {
        u32 op = *(const u32*)a;
        if ((op & 0x0FFF0FFF) != 0x0E070F90 || (op >> 28) == 0xF)
            continue;
        // Alloc halts when nothing fits: ask first
        if (!patchContext.GetPatchHeap().CanAlloc(SECTION_SIZE(autopilothalt)))
        {
            failed++;
            continue;
        }
        u32* stub = (u32*)patchContext.GetPatchHeap().Alloc(SECTION_SIZE(autopilothalt));
        memcpy(stub, SECTION_START(autopilothalt), SECTION_SIZE(autopilothalt));
        u32 offReturn = (u32)&autopilothalt_return - (u32)SECTION_START(autopilothalt);
        u32 offIdle = (u32)&autopilothalt_idle - (u32)SECTION_START(autopilothalt);
        u32 site = autoload ? autoload->AdjustInitialToFinal(a) : a; // where it runs
        u32 back = (u32)stub + offReturn;
        *(u32*)((u8*)stub + offReturn) = 0xEA000000 | (((site + 4) - (back + 8)) >> 2 & 0xFFFFFF);
        *(u32*)((u8*)stub + offIdle) = idle;
        *(u32*)a = (op & 0xF0000000) | 0x0A000000 | (((u32)stub - (site + 8)) >> 2 & 0xFFFFFF);
        count++;
    }
    LOG_DEBUG("Autopilot: %u halts timed (%u without room)\n", count, failed);
}
#endif

Arm9Patcher::PatchResult Arm9Patcher::ApplyPatches(const LoaderPlatform* loaderPlatform, const ApListEntry* apListEntry,
    bool isCloneBootRom, bool runInDSiMode, const loader_info_t* loaderInfo) const
{
    auto romHeader = (const nds_header_ntr_t*)TWL_SHARED_MEMORY->ntrSharedMem.romHeader;
    auto twlRomHeader = (const nds_header_twl_t*)TWL_SHARED_MEMORY->twlRomHeader;
    u32 arm9Size = romHeader->arm9Size;
    u32 arm9iSize = romHeader->SupportsDsiMode() ? twlRomHeader->arm9iSize : 0;
    u32 compressedEnd = 0;
    std::unique_ptr<IAutoloadAdjuster> arm9Autoload;
    std::unique_ptr<IAutoloadAdjuster> arm9iAutoload;
    module_params_ntr_t* moduleParams = nullptr;
    SdkVersion sdkVersion = 0u;
    if (romHeader->gameCode == GAMECODE("AS2E"))
    {
        // Spider-Man 2 (USA) is probably the only game without module params
        sdkVersion = 0x02004F50;
        arm9Autoload = std::make_unique<AutoloadAdjuster<autoload_list_entry_t>>(
            (autoload_list_entry_t*)0x0215DBA0,
            (autoload_list_entry_t*)0x0215DBB8,
            0x02157CC0);
    }
    else
    {
        moduleParams = ModuleParamsLocator().FindModuleParams(romHeader);
        if (moduleParams)
        {
            LOG_DEBUG("Module params found at 0x%p\n", moduleParams);
            sdkVersion = moduleParams->sdkVersion;
            LOG_DEBUG("Sdk version: 0x%x\n", sdkVersion);
            const u32* miiUncompressBackward = nullptr;
            if (moduleParams->compressedEnd)
            {
                miiUncompressBackward = FindMIiUncompressBackward(romHeader->arm9LoadAddress, sdkVersion);
                if (miiUncompressBackward)
                {
                    u32 originalBottom = *(u32*)(moduleParams->compressedEnd - 4);
                    // Some rom hacks decompress the arm9, but don't set compressedEnd to 0.
                    if (originalBottom < 4 * 1024 * 1024)
                    {
                        arm9Size = moduleParams->compressedEnd + originalBottom - romHeader->arm9LoadAddress;
                        ((uncompress_func_t)miiUncompressBackward)((void*)moduleParams->compressedEnd);
                    }
                    else
                    {
                        LOG_WARNING("Invalid originalBottom value found. Skipping arm9 decompression.\n");
                    }
                    compressedEnd = moduleParams->compressedEnd;
                    moduleParams->compressedEnd = 0;
                }
                else
                {
                    LOG_ERROR("MIi_UncompressBackward not found\n");
                }
            }
#ifndef RTS_CENSUS
            // the overlays the register shadow scans come compressed the same way
            ShadowInstall::SetUncompress(miiUncompressBackward ? miiUncompressBackward
                : FindMIiUncompressBackward(romHeader->arm9LoadAddress, sdkVersion));
#endif

            // the twl sdk's 4-word entries, except in its ds-only builds (golden
            // sun dark dawn), which keep the 3 words of before
            u32 listBytes = moduleParams->autoloadListEnd - moduleParams->autoloadListStart;
            bool sdk5Entries = sdkVersion.IsTwlSdk() && listBytes % sizeof(autoload_list_entry_sdk5_t) == 0
                && (romHeader->unitCode != 0 || listBytes % sizeof(autoload_list_entry_t) != 0);
            if (sdk5Entries)
            {
                arm9Autoload = std::make_unique<AutoloadAdjuster<autoload_list_entry_sdk5_t>>(
                    (autoload_list_entry_sdk5_t*)moduleParams->autoloadListStart,
                    (autoload_list_entry_sdk5_t*)moduleParams->autoloadListEnd,
                    moduleParams->autoloadStart);
            }
            else
            {
                arm9Autoload = std::make_unique<AutoloadAdjuster<autoload_list_entry_t>>(
                    (autoload_list_entry_t*)moduleParams->autoloadListStart,
                    (autoload_list_entry_t*)moduleParams->autoloadListEnd,
                    moduleParams->autoloadStart);
            }

            if (runInDSiMode)
            {
                auto arm9iModuleParams = (module_params_twl_t*)(romHeader->arm9LoadAddress + twlRomHeader->arm9iModuleParamsAddress);
                if (arm9iModuleParams->magicBigEndian == MODULE_PARAMS_TWL_MAGIC_BE &&
                    arm9iModuleParams->magicLittleEndian == MODULE_PARAMS_TWL_MAGIC_LE)
                {
                    if (arm9iModuleParams->compressedEnd != 0)
                    {
                        LOG_DEBUG("Compressed arm9i found\n");
                        if (miiUncompressBackward)
                        {
                            arm9iSize = arm9iModuleParams->compressedEnd + *(u32*)(arm9iModuleParams->compressedEnd - 4) - twlRomHeader->arm9iLoadAddress;
                            ((uncompress_func_t)miiUncompressBackward)((void*)arm9iModuleParams->compressedEnd);
                            arm9iModuleParams->compressedEnd = 0;
                            LOG_DEBUG("Decompressed arm9i\n");
                        }
                        else
                        {
                            LOG_ERROR("Could not decompress arm9i\n");
                        }
                    }
                    arm9iAutoload = std::make_unique<AutoloadAdjuster<autoload_list_entry_sdk5_t>>(
                        (autoload_list_entry_sdk5_t*)arm9iModuleParams->autoloadListStart,
                        (autoload_list_entry_sdk5_t*)arm9iModuleParams->autoloadListEnd,
                        arm9iModuleParams->autoloadStart);
                }
            }
        }
        else
        {
            LOG_ERROR("Module params not found!\n");
        }
    }
    LOG_DEBUG("Arm9 region: 0x%x - 0x%x\n", romHeader->arm9LoadAddress, romHeader->arm9LoadAddress + arm9Size);
    PatchContext patchContext
    {
        (void*)romHeader->arm9LoadAddress,
        arm9Size,
        std::move(arm9Autoload),
        romHeader->SupportsDsiMode() ? (void*)twlRomHeader->arm9iLoadAddress : nullptr,
        arm9iSize,
        std::move(arm9iAutoload),
        sdkVersion,
        romHeader->gameCode,
        romHeader->softwareVersion,
        loaderPlatform
    };
    PatchCollection patchCollection;
    OSResetSystemPatch* osResetSystemPatch = nullptr;
    ExitComboPatch* exitComboPatch = nullptr;
    bool isPokemonDownloader = false;
    bool exitComboEnabled = false;
    bool keepDsiBios = false;
    if (sdkVersion != 0)
    {
        if (*(vu32*)0x02FFF00C == GAMECODE("ADAJ") &&
            romHeader->arm9LoadAddress == 0x02004000 &&
            romHeader->arm9EntryAddress == 0x02004800)
        {
            isPokemonDownloader = true;
            // pokemon downloader
            patchContext.GetPatchHeap().AddFreeSpace((void*)0x023FF160, 0x6A0);
            patchCollection.AddPatch(new PokemonDownloaderArm9Patch(loaderInfo));
        }
        else
        {
            u32 availableParentSize = 0;
            if (isCloneBootRom)
            {
                availableParentSize = GetAvailableParentSectionSpace();
                LOG_DEBUG("0x%X bytes available in .parent section\n", availableParentSize);
            }

            if (availableParentSize >= REQUIRED_PATCH_HEAP_SPACE)
            {
                // All of the zero tail, not just the minimum: the exit combo and
                // the rts menu hook come after the mandatory patches and need more
                // (NSMB and Metroid Prime Hunters had no room with 0x500).
                patchContext.GetPatchHeap().AddFreeSpace(
                    (void*)(PARENT_SECTION_END - availableParentSize),
                    availableParentSize);
                LOG_DEBUG("Placing patches in .parent section\n");
            }
            else
            {
                SecureSysCallsUnusedSpaceLocator().FindUnusedSpace(romHeader, patchContext.GetPatchHeap());
            }
        }

        // The in-game exit covers retail games, DS or DSi mode, booted through
        // dldi with a launcher to go back to, which the arm7 only records when the
        // dldi driver in use is the one built into Pico Loader. Left out: DSiWare
        // (it lacks the card patches the reboot code builds on), the downloader,
        // whose patch heap at 0x023FF160 overlaps the exit request block, and the
        // games with a NAND save. DSi mode gets the basic rts (no shadow yet).
        exitComboEnabled = !isPokemonDownloader
            && !(sdkVersion.IsTwlSdk() && twlRomHeader->IsDsiWare())
            && !hasNandSave(romHeader->gameCode)
            && loaderInfo->picoLoaderBootDrive == PLOAD_BOOT_DRIVE_DLDI
            && TWL_SHARED_MEMORY->exitRequest.pathMagic == EXIT_REQUEST_PATH_MAGIC;

        if (sdkVersion.IsTwlSdk())
        {
            if (!twlRomHeader->IsDsiWare())
            {
                patchCollection.AddPatch(new CardiIsRomDmaAvailablePatch());
                patchCollection.AddPatch(new CardiReadRomWithCpuPatch());

                if (runInDSiMode)
                {
                    patchCollection.AddPatch(new CardiReadCardWithHashInternalAsyncPatch());
                }
            }
        }
        else
        {
            patchCollection.AddPatch(new CardiReadCardPatch());
            patchCollection.AddPatch(new CardiTryReadCardDmaPatch());
        }

        patchCollection.AddPatch(new CardiReadRomIdCorePatch());
        osResetSystemPatch = new OSResetSystemPatch(loaderInfo, runInDSiMode);
        patchCollection.AddPatch(osResetSystemPatch);
        AddGamePatches(patchCollection, romHeader->gameCode, apListEntry, exitComboEnabled && !runInDSiMode && !ShadowInstall::BasicMode());

        // Added last on purpose: this is an optional feature that must never
        // take patch heap a mandatory patch needs. Because ApplyPatch runs in
        // insertion order, every mandatory patch above allocates first and the
        // exit combo only claims what is left, or goes without.
        if (exitComboEnabled)
        {
            exitComboPatch = new ExitComboPatch(osResetSystemPatch);
            patchCollection.AddPatch(exitComboPatch);
        }

        if (moduleParams && compressedEnd != 0)
        {
            AddRestoreCompressedEndPatch(
                patchContext,
                romHeader->arm9AutoLoadDoneHookAddress,
                &moduleParams->compressedEnd,
                compressedEnd);
        }
    }
    if (!patchCollection.TryPerformPatches(patchContext))
    {
        ErrorDisplay().PrintError("Failed to apply arm9 patches.");
    }

#ifdef RTS_AUTOPILOT
    // the autopilot build: the game reads the autopilot's keys where it read KEYINPUT
    if (exitComboPatch != nullptr && exitComboPatch->IsInstalled() && !runInDSiMode
        && TWL_SHARED_MEMORY->autopilot.saveFrame != 0)
    {
        redirectKeyInput(romHeader->arm9LoadAddress, arm9Size,
            exitComboPatch->GetBlockAddress() + RTS_AUTOPILOT_OFFSET);
        installHaltTiming(patchContext, romHeader->arm9LoadAddress, arm9Size,
            exitComboPatch->GetBlockAddress() + RTS_AUTOPILOT_OFFSET + offsetof(rts_autopilot_t, idle));
    }
#endif

#ifndef RTS_CENSUS
    // the display register shadow for the rts, put in once the arm7's arena is known
    if (exitComboPatch != nullptr && exitComboPatch->IsInstalled() && !runInDSiMode
        && !ShadowInstall::BasicMode())
        ShadowInstall::Collect(romHeader->arm9LoadAddress, arm9Size, patchContext.GetAutoloadAdjuster());
    else
        ShadowInstall::Collect(0, 0, nullptr);
#endif

    // On a DSi or 3DS a game whose exit combo got installed keeps the bios of
    // the DSi, so the combo can return to dsi mode. Everything else gets the ds
    // bios as before: homebrew and DSi-enhanced roms in ds mode check scfg to
    // tell a DSi, which then reads as one.
    keepDsiBios = gIsDsiMode && exitComboPatch != nullptr && exitComboPatch->IsInstalled();
    if (keepDsiBios && !isCloneBootRom)
    {
        // The bios of the DSi has the lz77 callback decompression under another
        // swi number. Left alone in a clone boot rom, as nds-bootstrap does: the
        // thunk sits in the secure area, which the game sends as is to the
        // download play children it boots.
        DsiBiosLz77SwiPatch lz77SwiPatch(false);
        lz77SwiPatch.FindPatchTarget(patchContext);
        lz77SwiPatch.ApplyPatch(patchContext);
    }
#ifndef RTS_CENSUS
    // What the patch heap has left goes to the shadow's overlay slots, should its
    // space in the arm7's arena be short: zeros only (the tail of a clone boot
    // rom's .parent section), never filler a romhack might keep code in.
    if (exitComboPatch != nullptr && exitComboPatch->IsInstalled() && !runInDSiMode
        && !ShadowInstall::BasicMode())
    {
        u32 extraSize = patchContext.GetPatchHeap().LargestFree() & ~15u;
        if (extraSize >= 0x100)
        {
            u32* extra = (u32*)patchContext.GetPatchHeap().Alloc(extraSize);
            bool zeros = true;
            for (u32 i = 0; i < extraSize / 4 && zeros; i++)
                zeros = extra[i] == 0;
            // stub slots are 16 bytes, on 16
            u32 start = ((u32)extra + 15) & ~15u;
            u32 size = (extraSize - (start - (u32)extra)) & ~15u;
            if (zeros)
            {
                ShadowInstall::SetExtraSlots(start, size);
                LOG_DEBUG("Shadow: 0x%x bytes of spare patch heap at 0x%x\n", size, start);
            }
        }
    }
#endif
    dc_flushAll();
    dc_drainWriteBuffer();
    ic_invalidateAll();

    void** softResetCheatsPointer = nullptr;
    if (osResetSystemPatch != nullptr)
    {
        softResetCheatsPointer = osResetSystemPatch->GetCheatsPointerAtTarget();
    }

    return PatchResult
    {
        .softResetCheatsPointer = softResetCheatsPointer,
        .exitComboPatch = exitComboPatch != nullptr && exitComboPatch->IsInstalled() ? exitComboPatch : nullptr,
        .keepDsiBios = keepDsiBios
    };
}

const u32* Arm9Patcher::FindMIiUncompressBackward(u32 arm9LoadAddress, SdkVersion sdkVersion) const
{
    const u32* miiUncompressBackwardPattern;
    if (sdkVersion <= 0x2017532)
    {
        miiUncompressBackwardPattern = sMiiUncompressBackwardPatternOld;
    }
    else
    {
        miiUncompressBackwardPattern = sMiiUncompressBackwardPattern;
    }

    const u32* miiUncompressBackward = fastSearch16((const u32*)arm9LoadAddress, 0x1000, miiUncompressBackwardPattern);
    if (!sdkVersion.IsTwlSdk() && !miiUncompressBackward)
    {
        miiUncompressBackward = fastSearch16((const u32*)arm9LoadAddress, 0x1000, sMiiUncompressBackwardPatternOld2);
    }
    if (sdkVersion.IsTwlSdk() && !miiUncompressBackward)
    {
        miiUncompressBackward = fastSearch16((const u32*)arm9LoadAddress, 0x1000, sMiiUncompressBackwardPatternHybrid);
    }

    return miiUncompressBackward;
}

void Arm9Patcher::AddGamePatches(PatchCollection& patchCollection, u32 gameCode, const ApListEntry* apListEntry, bool shadowOverlays) const
{
    OverlayHookPatch* overlayHookPatch;
    if (gameCode == GAMECODE("BO5P") || gameCode == GAMECODE("BO5E") || gameCode == GAMECODE("BO5J"))
    {
        overlayHookPatch = new GoldenSunDarkDawnOverlayHookPatch();
        overlayHookPatch->AddOverlayPatch(new DSProtectOverlayPatch(334, 0, DSProtectVersion::v2_01, ~0u));
        overlayHookPatch->AddOverlayPatch(new DSProtectOverlayPatch(335, 0, DSProtectVersion::v2_01s, ~0u));
    }
    else
    {
        overlayHookPatch = new FsStartOverlayHookPatch();
        if (apListEntry)
        {
            AddDSProtectPatches(patchCollection, overlayHookPatch, apListEntry);
        }
        AddGameSpecificPatches(patchCollection, overlayHookPatch, gameCode);
    }
#ifndef RTS_CENSUS
    // the rts's register shadow, for the stores in the overlays: both hooks have
    // the overlay's table entry in r5, the overlay loaded and unpacked
    if (shadowOverlays)
        overlayHookPatch->AddOptionalOverlayPatch(new ShadowOverlayPatch());
#else
    (void)shadowOverlays;
#endif

    patchCollection.AddPatch(overlayHookPatch);
}

void Arm9Patcher::AddDSProtectPatches(
    PatchCollection& patchCollection, OverlayHookPatch* overlayHookPatch, const ApListEntry* apListEntry) const
{
    u32 regularOverlayId = apListEntry->GetRegularOverlayId();
    if (regularOverlayId != AP_LIST_OVERLAY_ID_INVALID)
    {
        if (regularOverlayId == AP_LIST_OVERLAY_ID_STATIC_ARM9)
        {
            patchCollection.AddPatch(new DSProtectArm9Patch(
                apListEntry->GetRegularOffset(), apListEntry->GetDSProtectVersion(),
                apListEntry->GetDSProtectFunctionMask()));
        }
        else
        {
            overlayHookPatch->AddOverlayPatch(new DSProtectOverlayPatch(
                regularOverlayId, apListEntry->GetRegularOffset(),
                apListEntry->GetDSProtectVersion(), apListEntry->GetDSProtectFunctionMask()));
        }
    }
    u32 sOverlayId = apListEntry->GetSOverlayId();
    if (sOverlayId != AP_LIST_OVERLAY_ID_INVALID)
    {
        auto version = apListEntry->GetDSProtectVersion();
        if (version < DSProtectVersion::v2_00s)
        {
            version = (DSProtectVersion)((u32)version - (u32)DSProtectVersion::v2_00 + (u32)DSProtectVersion::v2_00s);
        }
        if (sOverlayId == AP_LIST_OVERLAY_ID_STATIC_ARM9)
        {
            patchCollection.AddPatch(new DSProtectArm9Patch(
                apListEntry->GetSOffset(), version, ~0u));
        }
        else
        {
            overlayHookPatch->AddOverlayPatch(new DSProtectOverlayPatch(
                sOverlayId, apListEntry->GetSOffset(), version, ~0u));
        }
    }
}

void Arm9Patcher::AddGameSpecificPatches(
    PatchCollection& patchCollection, OverlayHookPatch* overlayHookPatch, u32 gameCode) const
{
    switch (gameCode)
    {
        // Dragon Ball: Origins 2
        case GAMECODE("BDBE"):
        {
            // BDBE;2;1.23;111111;0x1FC;-1;0x0
            // BDBE;3;1.23;111111;0x47DC;-1;0x0
            overlayHookPatch->AddOverlayPatch(new DSProtectOverlayPatch(2, 0x1FC, DSProtectVersion::v1_23, ~0u));
            overlayHookPatch->AddOverlayPatch(new DSProtectOverlayPatch(3, 0x47DC, DSProtectVersion::v1_23, ~0u));
            break;
        }
        case GAMECODE("BDBJ"):
        {
            // BDBJ;2;1.23;111111;0x1FC;-1;0x0
            // BDBJ;3;1.23;111111;0x4C34;-1;0x0
            overlayHookPatch->AddOverlayPatch(new DSProtectOverlayPatch(2, 0x1FC, DSProtectVersion::v1_23, ~0u));
            overlayHookPatch->AddOverlayPatch(new DSProtectOverlayPatch(3, 0x4C34, DSProtectVersion::v1_23, ~0u));
            break;
        }
        case GAMECODE("BDBP"):
        {
            // BDBP;2;1.23;111111;0x1FC;-1;0x0
            // BDBP;3;1.23;111111;0x484C;-1;0x0
            overlayHookPatch->AddOverlayPatch(new DSProtectOverlayPatch(2, 0x1FC, DSProtectVersion::v1_23, ~0u));
            overlayHookPatch->AddOverlayPatch(new DSProtectOverlayPatch(3, 0x484C, DSProtectVersion::v1_23, ~0u));
            break;
        }
        // Puyo Puyo 7
        case GAMECODE("BYOJ"):
        {
            // BYOJ;9;1.08;100110;1.08;0x21AC;-1;0x0
            // BYOJ;12;1.08;100101;1.08;0xC568;-1;0x0
            // BYOJ;14;1.08;010101;1.08;0x13AB8;-1;0x0
            // BYOJ;15;1.08;010110;1.08;0x16DF0;-1;0x0
            // BYOJ;19;1.08;011010;1.08;0x17F8;-1;0x0
            overlayHookPatch->AddOverlayPatch(new DSProtectPuyoPuyo7Patch());
            break;
        }
        // Pokemon HeartGold & SoulSilver
        case GAMECODE("IPGD"):
        case GAMECODE("IPGE"):
        case GAMECODE("IPGF"):
        case GAMECODE("IPGI"):
        case GAMECODE("IPGJ"):
        case GAMECODE("IPGK"):
        case GAMECODE("IPGS"):
        case GAMECODE("IPKD"):
        case GAMECODE("IPKE"):
        case GAMECODE("IPKF"):
        case GAMECODE("IPKI"):
        case GAMECODE("IPKJ"):
        case GAMECODE("IPKK"):
        case GAMECODE("IPKS"):
        {
            overlayHookPatch->AddOverlayPatch(new PokemonIrApPatch(PokemonIrVersion::Hgss));
            break;
        }
        // Pokemon Black & White
        case GAMECODE("IRAD"):
        case GAMECODE("IRAF"):
        case GAMECODE("IRAI"):
        case GAMECODE("IRAJ"):
        case GAMECODE("IRAK"):
        case GAMECODE("IRAO"):
        case GAMECODE("IRAS"):
        case GAMECODE("IRBD"):
        case GAMECODE("IRBF"):
        case GAMECODE("IRBI"):
        case GAMECODE("IRBJ"):
        case GAMECODE("IRBK"):
        case GAMECODE("IRBO"):
        case GAMECODE("IRBS"):
        {
            overlayHookPatch->AddOverlayPatch(new PokemonIrApPatch(PokemonIrVersion::Bw1));
            break;
        }
        // Pokemon Black & White 2
        case GAMECODE("IRDD"):
        case GAMECODE("IRDF"):
        case GAMECODE("IRDI"):
        case GAMECODE("IRDJ"):
        case GAMECODE("IRDK"):
        case GAMECODE("IRDO"):
        case GAMECODE("IRDS"):
        case GAMECODE("IRED"):
        case GAMECODE("IREF"):
        case GAMECODE("IREI"):
        case GAMECODE("IREJ"):
        case GAMECODE("IREK"):
        case GAMECODE("IREO"):
        case GAMECODE("IRES"):
        {
            overlayHookPatch->AddOverlayPatch(new PokemonIrApPatch(PokemonIrVersion::Bw2));
            break;
        }
        // WarioWare: D.I.Y.
        case GAMECODE("UORE"):
        case GAMECODE("UORP"):
        case GAMECODE("UORJ"):
        {
            patchCollection.AddPatch(new WarioWareDiyNandSavePatch());
            break;
        }
        // Jam with the Band
        case GAMECODE("UXBP"):
        {
            patchCollection.AddPatch(new JamWithTheBandNandSavePatch());
            break;
        }
        // Face Training
        case GAMECODE("USKV"):
        {
            patchCollection.AddPatch(new FaceTrainingNandSavePatch());
            break;
        }
        // Nintendo DS Guide
        case GAMECODE("UGDA"):
        {
            patchCollection.AddPatch(new NintendoDSGuideNandSavePatch());
            break;
        }
        // Last Window: The Secret of Cape West
        case GAMECODE("YLUP"):
        {
            patchCollection.AddPatch(new LastWindowCrcPatch());
            break;
        }
        // Rabbids Go Home
        case GAMECODE("VRGE"):
        case GAMECODE("VRGV"):
        {
            overlayHookPatch->AddOverlayPatch(new RabbidsGoHomePatch());
            break;
        }
        // Kirby Super Star Ultra
        case GAMECODE("YKWE"):
        case GAMECODE("YKWJ"):
        case GAMECODE("YKWK"):
        case GAMECODE("YKWP"):
        {
            overlayHookPatch->AddOverlayPatch(new KirbySuperStarUltraPatch());
            break;
        }
    }
}

void Arm9Patcher::AddRestoreCompressedEndPatch(PatchContext& patchContext,
    u32 arm9AutoLoadDoneHookAddress, u32* moduleParamsCompressedEnd, u32 originalCompressedEndValue) const
{
    // Restore compressedEnd after first boot.
    // This is necessary to not break cloneboot.
    const u32 compressedEndFixCode[] =
    {
        0xE59F0014, // ldr r0,= moduleParamsCompressedEnd
        0xE59F1014, // ldr r1,= originalCompressedEndValue
        0xE5801000, // str r1, [r0]
        0xE59F0010, // ldr r0,= arm9AutoLoadDoneHookAddress
        0xE59F1000, // ldr r1, ret
        0xE5801000, // str r1, [r0]
        0xE12FFF1E, // ret: bx lr
        (u32)moduleParamsCompressedEnd,
        originalCompressedEndValue,
        arm9AutoLoadDoneHookAddress
    };

    void* fixDst = patchContext.GetPatchHeap().Alloc(sizeof(compressedEndFixCode));
    memcpy(fixDst, compressedEndFixCode, sizeof(compressedEndFixCode));
    *(u32*)arm9AutoLoadDoneHookAddress = 0xEA000000u | ((((int)fixDst - (int)arm9AutoLoadDoneHookAddress - 8) >> 2) & 0xFFFFFF);
}

u32 Arm9Patcher::GetAvailableParentSectionSpace() const
{
    u32 availableParentSize = 0;
    for (u32 ptr = PARENT_SECTION_END; ptr > PARENT_SECTION_START; ptr -= 32)
    {
        u32* segment = (u32*)(ptr - 32);
        if (segment[0] != 0 || segment[1] != 0 || segment[2] != 0 || segment[3] != 0 ||
            segment[4] != 0 || segment[5] != 0 || segment[6] != 0 || segment[7] != 0)
        {
            break;
        }

        availableParentSize += 32;
    }

    return availableParentSize;
}
