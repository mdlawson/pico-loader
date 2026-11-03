#include "common.h"
#include "patches/PatchContext.h"
#include "patches/platform/LoaderPlatform.h"
#include "sharedMemory.h"
#include "LoaderInfo.h"
#include "OSResetSystemPatchCode.h"
#include "ExitComboPatchCode.h"
#include "ExitComboPatch.h"
#include "CensusPatchCode.h"

// ExitComboPatchCode.s reaches resetParam and the sdk card lock word from the exit
// request block. Both live in the same shared page in either mirror, so the distances
// are fixed.
static_assert(offsetof(shared_memory_twl_t, ntrSharedMem) + offsetof(shared_memory_ntr_t, resetParam)
    - offsetof(shared_memory_twl_t, exitRequest) == 0x5A0);
static_assert(offsetof(shared_memory_twl_t, ntrSharedMem) + offsetof(shared_memory_ntr_t, slot1Lock)
    - offsetof(shared_memory_twl_t, exitRequest) == 0x960);
// and the bios's debug vector, the census's way in
static constexpr u32 sDebugVectorFromBlock = offsetof(shared_memory_twl_t, ntrSharedMem) +
    offsetof(shared_memory_ntr_t, arm9ExceptionVector) - offsetof(shared_memory_twl_t, exitRequest);
static_assert(sDebugVectorFromBlock == 0x71C);

// Tail of the SDK IRQ dispatcher: ldr r1, [pc, #8]; ldr r0, [r1, r0, lsl #2];
// ldr lr, [pc, #4]; bx r0. The irq table address and the irq return address
// literals follow directly after these four instructions. This four-word
// table-dispatch-and-branch sequence is highly distinctive, so it is searched
// for directly rather than anchoring on the dispatcher prologue, whose exact
// bytes vary across SDK versions.
static const u32 sIrqDispatchTailPattern[] = { 0xE59F1008u, 0xE7910100u, 0xE59FE004u, 0xE12FFF10u };

// Retail games fill the gaps between the secure area system calls with random bytes, but
// some romhacks keep code of their own there. These are words such code jumps or returns
// with, which the filler of the retail games checked never holds.
static bool HoldsArmBranch(const void* start, u32 size)
{
    const u32* words = (const u32*)start;
    for (u32 i = 0; i < size / 4; i++)
    {
        u32 word = words[i];
        if (word == 0xE51FF004 ||                // ldr pc, [pc, #-4]
            (word & 0xFFFFF000) == 0xE59FF000 || // ldr pc, [pc, #imm]
            (word & 0xFFFFFFF0) == 0xE12FFF10 || // bx rN
            (word & 0xFFFF8000) == 0xE8BD8000)   // pop {..., pc}
        {
            return true;
        }
    }
    return false;
}

bool ExitComboPatch::FindPatchTarget(PatchContext& patchContext)
{
    _dispatchTail = patchContext.FindPattern32(sIrqDispatchTailPattern, sizeof(sIrqDispatchTailPattern));
    if (_dispatchTail)
    {
        LOG_DEBUG("Irq dispatch tail found at 0x%p\n", _dispatchTail);
    }
    else
    {
        LOG_DEBUG("Irq dispatch tail not found\n");
    }
    return true; // optional patch
}

void ExitComboPatch::ApplyPatch(PatchContext& patchContext)
{
    if (!_dispatchTail)
        return;

    // A game without an OS_ResetSystem of its own gets the reboot code from here.
    // That code calls the platform sd read code, which the card read patches have
    // already put in on a platform that reads the rom from the sd card. Otherwise
    // it would take patch heap that is not counted below.
    auto& patchCodes = patchContext.GetPatchCodeCollection();
    bool emitRebootCode = !_osResetSystemPatch->HasTarget();
    if (emitRebootCode && !patchContext.GetLoaderPlatform()->HasSdReadPatchCode(patchCodes))
    {
        LOG_DEBUG("No sd read code for the reboot code, no exit combo\n");
        return;
    }

    // Everything this patch takes from the patch heap, in the order it is taken
    // below. It only gets what the mandatory patches left, and all of it or
    // nothing: Alloc halts when nothing fits, so the whole sequence is tried first.
    const u32 sizes[] =
    {
        // OSResetSystemPatch::EmitRebootCode
        sizeof(loader_info_t),
        SECTION_SIZE(patch_osresetsystem_boot),
        SECTION_SIZE(patch_osresetsystem),
        // each piece before the one that jumps to it
        SECTION_SIZE(patch_exitcombo_teardown),
        SECTION_SIZE(patch_exitcombo_wait),
        SECTION_SIZE(patch_exitcombo_check),
        SECTION_SIZE(patch_exitcombo_hook)
    };
    constexpr u32 sizeCount = sizeof(sizes) / sizeof(sizes[0]);
    u32 first = emitRebootCode ? 0 : 3;
    void* addresses[sizeCount];
    auto& patchHeap = patchContext.GetPatchHeap();
    if (!patchHeap.CanAllocInOrder(&sizes[first], sizeCount - first, &addresses[first]))
    {
        LOG_DEBUG("No room for the exit combo\n");
        return;
    }

    // The mandatory patches took what they needed already; this takes more of the
    // same gaps, so leave the exit out rather than write over a romhack's code.
    for (u32 i = first; i < sizeCount; i++)
    {
        if (HoldsArmBranch(addresses[i], sizes[i]))
        {
            LOG_DEBUG("Exit combo space holds code, left out\n");
            return;
        }
    }

    // Keep what the game has where all of it goes, to put it back should the arm7
    // half not make it. Before anything goes in: loader_info_t is copied over as
    // soon as it is allocated. Every size is a whole number of words.
    u32 savedWords = 0;
    for (u32 i = first; i < sizeCount; i++)
        savedWords += sizes[i] / 4;
    _savedGameWords = new u32[savedWords];
    _savedRegionCount = 0;
    savedWords = 0;
    for (u32 i = first; i < sizeCount; i++)
    {
        _savedRegionAddress[_savedRegionCount] = (u32)addresses[i];
        _savedRegionSize[_savedRegionCount++] = sizes[i];
        for (u32 j = 0; j < sizes[i] / 4; j++)
            _savedGameWords[savedWords++] = ((const u32*)addresses[i])[j];
    }

    if (emitRebootCode)
    {
        _osResetSystemPatch->EmitRebootCode(patchContext);
    }
    auto rebootCode = _osResetSystemPatch->GetRebootCode();

    u32 blockAddress = (u32)&TWL_SHARED_MEMORY->exitRequest;
    if (!patchContext.GetSdkVersion().IsTwlSdk())
    {
        // SDK2-4 games access shared memory through the NTR mirror
        blockAddress -= 0x800000;
    }

    auto teardown = patchCodes.AddUniquePatchCode<ExitComboTeardownPatchCode>(patchHeap,
        rebootCode->GetPoolAtTarget(), rebootCode->GetBootLoaderBinariesFunction());
    auto wait = patchCodes.AddUniquePatchCode<ExitComboWaitPatchCode>(patchHeap, teardown);
    auto check = patchCodes.AddUniquePatchCode<ExitComboCheckPatchCode>(patchHeap, wait);
    auto hook = patchCodes.AddUniquePatchCode<ExitComboHookPatchCode>(patchHeap, blockAddress, check,
        _dispatchTail[4],  // irq table address literal
        _dispatchTail[5]); // irq return address literal

    LOG_DEBUG("Exit combo hook 0x%p, check 0x%p, wait 0x%p, teardown 0x%p\n", hook->GetEntryFunction(),
        check->GetEntryFunction(), wait->GetEntryFunction(), teardown->GetEntryFunction());
    // Each entry is the start of its piece, so this only fails if Alloc ever stops
    // doing what CanAllocInOrder does. The pieces are copied in anyway, but without
    // the hook nothing reaches them, and what was saved is not what they cover.
    if (teardown->GetEntryFunction() != addresses[3] || wait->GetEntryFunction() != addresses[4] ||
        check->GetEntryFunction() != addresses[5] || hook->GetEntryFunction() != addresses[6])
    {
        LOG_WARNING("Exit combo pieces not where planned, left out\n");
        delete[] _savedGameWords;
        _savedGameWords = nullptr;
        return;
    }

    _savedTail[0] = _dispatchTail[0];
    _savedTail[1] = _dispatchTail[1];
    _hookEntry = (u32)hook->GetEntryFunction();
    _censusLiteral = hook->GetCensusLiteralAtTarget();
    _blockAddress = blockAddress;
    _twlSdk = patchContext.GetSdkVersion().IsTwlSdk() ? 1 : 0;
    _dispatchTail[0] = 0xE51FF004; // ldr pc,= address
    _dispatchTail[1] = _hookEntry;
    _installed = 1;

    LOG_DEBUG("Exit combo enabled\n");

    // The census gate, only if there is room left after everything else: the
    // census goes without it, the exit combo never does.
    _censusGateTarget = nullptr;
    _censusGateAddress = 0;
    u32 gateSize = SECTION_SIZE(patch_census_gate);
    void* gateAddress = nullptr;
    static_assert(sizeof(_censusGateSaved) >= 44);
#ifndef RTS_CENSUS
    if (false)
#else
    if (!_twlSdk && gateSize <= sizeof(_censusGateSaved)
        && patchHeap.CanAllocInOrder(&gateSize, 1, &gateAddress)
        && !HoldsArmBranch(gateAddress, gateSize))
#endif
    {
        for (u32 j = 0; j < gateSize / 4; j++)
            _censusGateSaved[j] = ((const u32*)gateAddress)[j];
        auto gate = patchCodes.AddUniquePatchCode<CensusGatePatchCode>(patchHeap);
        if (gate->GetEntryFunction() == gateAddress)
        {
            _censusGateAddress = (u32)gateAddress;
            _censusGateEntry = (u32)gate->GetEntryFunction();
            _censusGateTarget = gate->GetTargetLiteralAtTarget();
            LOG_DEBUG("Census gate 0x%p\n", gateAddress);
        }
    }
}

bool ExitComboPatch::InstallCensus(void* space, u32 runAddress)
{
    if (!_installed || !_censusLiteral || !_censusGateTarget || _twlSdk || !space || !runAddress)
        return false;
    u32 size = SECTION_SIZE(patch_census);
    if (size > CENSUS_ARRAYS)
    {
        LOG_WARNING("Census code too big\n");
        return false;
    }
    u32* words = (u32*)space;
    for (u32 i = 0; i < CENSUS_SPACE_SIZE / 4; i++)
        words[i] = 0;
    const u32* code = (const u32*)SECTION_START(patch_census);
    for (u32 i = 0; i < size / 4; i++)
        words[i] = code[i];
    u32 vector = _blockAddress + sDebugVectorFromBlock;
    words[CENSUS_V_WANT / 4] = 1;
    words[CENSUS_V_VECADDR / 4] = vector;
    words[CENSUS_V_BSTACK / 4] = vector - 0x10; // the bios's control, spsr, r12, lr
    words[CENSUS_V_ARRAYS / 4] = runAddress + CENSUS_ARRAYS;
    words[CENSUS_V_MAGIC / 4] = CENSUS_MAGIC;
    // the hook calls the gate, which opens the sdk's region 2 and goes on into the census
    *_censusGateTarget = runAddress + ((u32)&patch_census_tick_entry - (u32)SECTION_START(patch_census));
    *_censusLiteral = _censusGateEntry;
    LOG_DEBUG("Census at 0x%x, written at 0x%p\n", runAddress, space);
    return true;
}

void ExitComboPatch::Deactivate()
{
    if (!_installed)
        return;

    // the hook only if it still jumps into the pieces
    if (_dispatchTail[0] == 0xE51FF004 && _dispatchTail[1] == _hookEntry)
    {
        _dispatchTail[0] = _savedTail[0];
        _dispatchTail[1] = _savedTail[1];
    }

    // and what the game had where the pieces and the reboot code went
    u32 savedWords = 0;
    for (u32 i = 0; i < _savedRegionCount; i++)
    {
        u32* region = (u32*)_savedRegionAddress[i];
        for (u32 j = 0; j < _savedRegionSize[i] / 4; j++)
            region[j] = _savedGameWords[savedWords++];
    }

    if (_censusGateAddress)
    {
        u32* region = (u32*)_censusGateAddress;
        for (u32 j = 0; j < SECTION_SIZE(patch_census_gate) / 4; j++)
            region[j] = _censusGateSaved[j];
        _censusGateAddress = 0;
    }

    delete[] _savedGameWords;
    _savedGameWords = nullptr;
    _installed = 0;
    _censusLiteral = nullptr;
    _censusGateTarget = nullptr;

    LOG_DEBUG("Exit combo taken back out\n");
}
