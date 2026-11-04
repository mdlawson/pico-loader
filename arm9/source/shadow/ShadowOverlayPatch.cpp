#include "common.h"
#include <string.h>
#include "sections.h"
#include "patches/PatchContext.h"
#include "ShadowInstall.h"
#include "ShadowOverlayPatch.h"

DEFINE_SECTION_SYMBOLS(shadowovlhook);
extern "C" void shadowovlhook_entry(void);
extern "C" u32 shadowovlhook_target;
extern "C" u32 shadowovlhook_next;

const void* ShadowOverlayPatch::InsertPatch(PatchContext& patchContext)
{
    shadowovlhook_next = next ? (u32)next->InsertPatch(patchContext) : 0;
    shadowovlhook_target = 0; // ShadowInstall::Install puts it in
    u32 size = SECTION_SIZE(shadowovlhook);
    void* address = patchContext.GetPatchHeap().Alloc(size);
    if (!address)
        return (const void*)shadowovlhook_next;
    memcpy(address, SECTION_START(shadowovlhook), size);
    ShadowInstall::SetOverlayHookTarget((u32*)((u8*)address + ((u32)&shadowovlhook_target - (u32)SECTION_START(shadowovlhook))));
    return (const u8*)address + ((u32)&shadowovlhook_entry - (u32)SECTION_START(shadowovlhook));
}
