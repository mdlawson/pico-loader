#pragma once
#include "../PatchCode.h"
#include "sections.h"
#include "sharedMemory.h"

DEFINE_SECTION_SYMBOLS(patch_exitcombo_hook);
DEFINE_SECTION_SYMBOLS(patch_census_gate);

extern "C" void patch_census_gate_entry();
extern u32 patch_census_gate_target;
DEFINE_SECTION_SYMBOLS(patch_exitcombo_check);
DEFINE_SECTION_SYMBOLS(patch_exitcombo_wait);
DEFINE_SECTION_SYMBOLS(patch_exitcombo_teardown);

extern "C" void patch_exitcombo_hook_entry(void);
extern "C" void patch_exitcombo_check_entry(void);
extern "C" void patch_exitcombo_wait_entry(void);
extern "C" void patch_exitcombo_teardown_entry(void);

extern u32 patch_exitcombo_hook_block_address;
extern u32 patch_exitcombo_hook_check_address;
extern u32 patch_exitcombo_hook_table_address;
extern u32 patch_exitcombo_hook_return_address;
extern u32 patch_exitcombo_hook_census_address;
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

/// @brief Waits for the arm7 to park, or takes the request back when it cancels.
///        A freeze ends with a cancel as well, once the arm7 has held for its second.
class ExitComboWaitPatchCode : public PatchCode
{
public:
    ExitComboWaitPatchCode(PatchHeap& patchHeap, const ExitComboTeardownPatchCode* teardown)
        : PatchCode(SECTION_START(patch_exitcombo_wait), SECTION_SIZE(patch_exitcombo_wait), patchHeap)
    {
        patch_exitcombo_wait_teardown_address = (u32)teardown->GetEntryFunction();
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
        patch_exitcombo_hook_block_address = blockAddress;
        patch_exitcombo_hook_check_address = (u32)check->GetEntryFunction();
        patch_exitcombo_hook_table_address = irqTableAddress;
        patch_exitcombo_hook_return_address = irqReturnAddress;
        patch_exitcombo_hook_census_address = 0;
    }

    /// @brief Where the hook's census literal landed, for the loader to fill
    ///        once the census space is known.
    u32* GetCensusLiteralAtTarget() const
    {
        return (u32*)GetAddressAtTarget((void*)&patch_exitcombo_hook_census_address);
    }

    const void* GetEntryFunction() const
    {
        return GetAddressAtTarget((void*)patch_exitcombo_hook_entry);
    }
};

/// @brief Opens the sdk's region 2 for the arm9, then goes on into the census.
class CensusGatePatchCode : public PatchCode
{
public:
    explicit CensusGatePatchCode(PatchHeap& patchHeap)
        : PatchCode(SECTION_START(patch_census_gate), SECTION_SIZE(patch_census_gate), patchHeap)
    {
        patch_census_gate_target = 0;
    }

    u32* GetTargetLiteralAtTarget() const
    {
        return (u32*)GetAddressAtTarget((void*)&patch_census_gate_target);
    }

    const void* GetEntryFunction() const
    {
        return GetAddressAtTarget((void*)patch_census_gate_entry);
    }
};
