#include "common.h"
#include <string.h>
#include "sections.h"
#include "moduleParams.h"
#include "AutoloadAdjuster.h"
#include "patches/arm7/DsiBiosPitchTablePatch.h"
#include "ShadowScan.h"
#include "ShadowInstall7.h"

DEFINE_SECTION_SYMBOLS(patch_shadow7);
extern "C" void shadow7_record(void);

#define SHADOW7_STUB_SIZE   16
#define SHADOW7_MAX_SITES   64

// Words all: the loader runs from vram, which takes no byte writes.
struct Entry7
{
    u32 initial; // where the loader patches
    u32 final;   // where it runs
    u32 orig;
};

static Entry7 sEntries[SHADOW7_MAX_SITES];
static u32 sEntryCount;
static u32 sRunAddress;

// The recorder reads the store's registers from r0-r12 as it saved them, and lr
// from the stub's copy; never sp or pc. Arm stores only (str, strb, strh, stm).
static bool recordable(u32 op)
{
    if ((op & 0x0E000000) == 0x08000000)
        return ((op >> 16) & 0xF) < 13 && !(op & 0xE000);
    u32 rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF;
    if (rn == 13 || rn == 15 || rd == 13 || rd == 15)
        return false;
    if ((op & 0x0C000000) == 0x04000000)
        return !(op & (1 << 20)); // str, strb
    // strh only: no strd on the arm7
    return (op & 0x0E1000F0) == 0x000000B0;
}

void ShadowInstall7::Collect(const nds_header_ntr_t* romHeader)
{
    sEntryCount = 0;
    sRunAddress = 0;
    u32 start = romHeader->arm7LoadAddress, size = romHeader->arm7Size;
    if (start < 0x02000000 || start + size > 0x02400000)
        return;
    // the sdk puts the sound driver in arm7 wram: the autoloads move it there
    auto params = findArm7ModuleParams(romHeader);
    if (!params)
    {
        LOG_DEBUG("Shadow7: no arm7 module params\n");
        return;
    }
    AutoloadAdjuster<autoload_list_entry_t> autoload(
        (const autoload_list_entry_t*)params->autoloadListStart,
        (const autoload_list_entry_t*)params->autoloadListEnd,
        params->autoloadStart);

    static u32 found[SHADOW7_MAX_SITES];
    shadow_scan::gTargetArm7 = 1;
    shadow_scan::Block block = { (const uint8_t*)start, start, size };
    shadow_scan::Scanner scanner(&block, 1, found, SHADOW7_MAX_SITES);
    scanner.Scan(0, 1);
    shadow_scan::gTargetArm7 = 0;

    u32 thumb = 0, odd = 0;
    for (u32 i = 0; i < scanner.SiteCount() && sEntryCount < SHADOW7_MAX_SITES; i++)
    {
        u32 site = found[i];
        if (site & 1)
        {
            thumb++;
            continue;
        }
        u32 op = *(const u32*)site;
        if (!recordable(op))
        {
            odd++;
            continue;
        }
        Entry7& e = sEntries[sEntryCount++];
        e.initial = site;
        e.final = autoload.AdjustInitialToFinal(site);
        e.orig = op;
    }
    LOG_DEBUG("Shadow7: %u arm sites, %u thumb and %u others left out, overflow %u\n",
        sEntryCount, thumb, odd, scanner.Overflow());
}

u32 ShadowInstall7::SpaceNeeded()
{
    if (sEntryCount == 0)
        return 0;
    return ((SECTION_SIZE(patch_shadow7) + 31) & ~31u) + sEntryCount * SHADOW7_STUB_SIZE;
}

static bool armBranch(u32 from, u32 to, u32& field)
{
    s32 d = (s32)(to - (from + 8));
    if (d < -0x2000000 || d >= 0x2000000)
        return false;
    field = ((u32)d >> 2) & 0xFFFFFF;
    return true;
}

bool ShadowInstall7::Install(void* space, u32 runAddress, u32 size)
{
    u32 code = (SECTION_SIZE(patch_shadow7) + 31) & ~31u;
    if (sEntryCount == 0 || size < code + SHADOW7_STUB_SIZE || (runAddress & 31))
        return false;
    memcpy(space, SECTION_START(patch_shadow7), SECTION_SIZE(patch_shadow7));
    u32 record = runAddress + ((u32)shadow7_record - (u32)SECTION_START(patch_shadow7));
    u32 made = 0, failed = 0;
    for (u32 i = 0; i < sEntryCount && code + (made + 1) * SHADOW7_STUB_SIZE <= size; i++)
    {
        const Entry7& e = sEntries[i];
        u32* site = (u32*)e.initial;
        u32 stubRun = runAddress + code + made * SHADOW7_STUB_SIZE;
        u32* stub = (u32*)((u8*)space + code + made * SHADOW7_STUB_SIZE);
        u32 toStub, toRecord;
        // a loader patch may have taken the instruction since the scan
        if (*site != e.orig || !armBranch(e.final, stubRun, toStub) || !armBranch(stubRun + 8, record, toRecord))
        {
            failed++;
            continue;
        }
        stub[0] = e.orig;
        stub[1] = 0xE52DE008; // str lr, [sp, #-8]!
        stub[2] = 0xEB000000 | toRecord; // bl shadow7_record
        stub[3] = e.final + 4;
        *site = (e.orig & 0xF0000000) | 0x0A000000 | toStub; // b<cond> stub
        made++;
    }
    u32* header = (u32*)space;
    header[2] = made;
    header[3] = code;
    sRunAddress = made ? runAddress : 0;
    LOG_DEBUG("Shadow7 at 0x%x: %u stubs of %u (%u left out)\n", runAddress, made, sEntryCount, failed);
    return made != 0;
}

u32 ShadowInstall7::RunAddress()
{
    return sRunAddress;
}
