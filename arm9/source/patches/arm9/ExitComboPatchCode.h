#pragma once
#include "../PatchCode.h"
#include "sections.h"
#include "sharedMemory.h"

DEFINE_SECTION_SYMBOLS(patch_exitcombo_hook);
DEFINE_SECTION_SYMBOLS(patch_exitcombo_check);
DEFINE_SECTION_SYMBOLS(patch_exitcombo_wait);
DEFINE_SECTION_SYMBOLS(patch_exitcombo_teardown);
DEFINE_SECTION_SYMBOLS(patch_exitcombo_hold);

extern "C" void patch_exitcombo_hook_entry(void);
extern "C" void patch_exitcombo_check_entry(void);
extern "C" void patch_exitcombo_wait_entry(void);
extern "C" void patch_exitcombo_teardown_entry(void);
extern "C" void patch_exitcombo_hold_entry(void);

extern u32 patch_exitcombo_hook_block_address;
extern u32 patch_exitcombo_hook_check_address;
extern u32 patch_exitcombo_hook_table_address;
extern u32 patch_exitcombo_hook_return_address;
extern u32 patch_exitcombo_hook_freeze_magic;
extern u32 patch_exitcombo_wait_hold_address;
extern u32 patch_exitcombo_check_wait_address;
extern u32 patch_exitcombo_wait_teardown_address;
extern u32 patch_exitcombo_teardown_resetpool_address;
extern u32 patch_exitcombo_teardown_bootloaderbinaries_address;

/// @brief Last piece of the exit: takes the card back, stops the dma and enters
///        the reboot code.
class ExitComboTeardownPatchCode : public PatchCode
{
public:
    ExitComboTeardownPatchCode(PatchHeap& patchHeap, const void* resetPool, const void* bootLoaderBinaries)
        : PatchCode(SECTION_START(patch_exitcombo_teardown), SECTION_SIZE(patch_exitcombo_teardown), patchHeap)
    {
        patch_exitcombo_teardown_resetpool_address = (u32)resetPool;
        patch_exitcombo_teardown_bootloaderbinaries_address = (u32)bootLoaderBinaries;
    }

    const void* GetEntryFunction() const
    {
        return GetAddressAtTarget((void*)patch_exitcombo_teardown_entry);
    }
};

/// @brief Waits for the arm7 to park, hold or cancel.
class ExitComboWaitPatchCode : public PatchCode
{
public:
    ExitComboWaitPatchCode(PatchHeap& patchHeap, const ExitComboTeardownPatchCode* teardown)
        : PatchCode(SECTION_START(patch_exitcombo_wait), SECTION_SIZE(patch_exitcombo_wait), patchHeap)
    {
        patch_exitcombo_wait_teardown_address = (u32)teardown->GetEntryFunction();
        patch_exitcombo_wait_hold_address = 0; // set in place by ExitComboPatch::EnableFreeze
    }

    /// @brief Where the copy in the game keeps the hold piece's address.
    u32* GetHoldAddressLiteral() const
    {
        return (u32*)GetAddressAtTarget(&patch_exitcombo_wait_hold_address);
    }

    const void* GetEntryFunction() const
    {
        return GetAddressAtTarget((void*)patch_exitcombo_wait_entry);
    }
};

/// @brief Waits for a vblank between card operations, then asks the arm7 to stop.
class ExitComboCheckPatchCode : public PatchCode
{
public:
    ExitComboCheckPatchCode(PatchHeap& patchHeap, const ExitComboWaitPatchCode* wait)
        : PatchCode(SECTION_START(patch_exitcombo_check), SECTION_SIZE(patch_exitcombo_check), patchHeap)
    {
        patch_exitcombo_check_wait_address = (u32)wait->GetEntryFunction();
    }

    const void* GetEntryFunction() const
    {
        return GetAddressAtTarget((void*)patch_exitcombo_check_entry);
    }
};

/// @brief The piece the irq dispatcher jumps to: looks for the combo on a vblank
///        and runs the displaced dispatcher tail.
class ExitComboHookPatchCode : public PatchCode
{
public:
    ExitComboHookPatchCode(PatchHeap& patchHeap, u32 blockAddress, const ExitComboCheckPatchCode* check,
        u32 irqTableAddress, u32 irqReturnAddress)
        : PatchCode(SECTION_START(patch_exitcombo_hook), SECTION_SIZE(patch_exitcombo_hook), patchHeap)
    {
        // a value the arm7 never publishes, until ExitComboPatch::EnableFreeze
        patch_exitcombo_hook_freeze_magic = 0xFFFFFFFF;
        patch_exitcombo_hook_block_address = blockAddress;
        patch_exitcombo_hook_check_address = (u32)check->GetEntryFunction();
        patch_exitcombo_hook_table_address = irqTableAddress;
        patch_exitcombo_hook_return_address = irqReturnAddress;
    }

    const void* GetEntryFunction() const
    {
        return GetAddressAtTarget((void*)patch_exitcombo_hook_entry);
    }

    /// @brief Where the copy in the game keeps the freeze combo magic it reacts to.
    u32* GetFreezeMagicLiteral() const
    {
        return (u32*)GetAddressAtTarget(&patch_exitcombo_hook_freeze_magic);
    }
};
