#include "common.h"
#include <string.h>
#include "sections.h"
#include "IAutoloadAdjuster.h"
#include "ShadowScan.h"
#include "ShadowInstall.h"

DEFINE_SECTION_SYMBOLS(patch_shadow);
extern "C" void shadow_record(void);

#define SHADOW_STUBS_OFFSET_FIELD   0x0C
#define SHADOW_COUNT_FIELD          0x08
#define SHADOW_STUB_SIZE            16
#define SHADOW_MAX_SITES            512
#define SHADOW_MAX_REGION2          8

static u32 sSites[SHADOW_MAX_SITES];      // initial addresses (where the loader patches)
static u32 sFinal[SHADOW_MAX_SITES];      // where they run
static u32 sSiteCount;
static u32 sThumbSites;
static u32 sRegion2Literal[SHADOW_MAX_REGION2]; // literal words to clear
static u32 sRegion2LiteralCount;
static u32 sRegion2Mcr[SHADOW_MAX_REGION2];     // setter mcr instructions to drop
static u32 sRegion2McrCount;
static u32 sOddRegs;
static u32 sRegion2Ok; // a word: the loader runs from vram, which takes no byte writes

// The recorder reads the store's registers from r0-r12 as it saved them.
static bool usesHighRegs(u32 op)
{
    if ((op & 0x0E000000) == 0x08000000)
        return ((op >> 16) & 0xF) >= 13 || (op & 0xE000);
    u32 rd = (op >> 12) & 0xF;
    bool dual = (op & 0x0E000000) == 0 && (op & 0x60) == 0x60; // strd: rd and rd + 1
    return ((op >> 16) & 0xF) >= 13 || rd >= 13 || (dual && rd >= 12);
}

void ShadowInstall::Collect(u32 arm9Start, u32 arm9Size, const IAutoloadAdjuster* autoload)
{
    sSiteCount = 0;
    sThumbSites = 0;
    sRegion2LiteralCount = 0;
    sRegion2McrCount = 0;
    sOddRegs = 0;
    sRegion2Ok = false;
    if (arm9Size < 0x1000 || arm9Size > 0x400000)
        return;

    // The sdk's region 2 setup: the startup code loads it from a literal pool
    // (ldr rX, =value / mcr p15, 0, rX, c6, c2, 0), and OS_SetProtectionRegion2
    // is mcr p15, 0, r0, c6, c2, 0 / bx lr. Both go: region 2 stays off.
    const u32* code = (const u32*)arm9Start;
    u32 words = arm9Size / 4;
    bool unknown = false;
    for (u32 i = 1; i + 1 < words; i++)
    {
        u32 w = code[i];
        if ((w & 0xFFFF0FFF) != 0xEE060F12)
            continue;
        u32 rx = (w >> 12) & 0xF;
        u32 p = code[i - 1];
        if ((p & 0x0F7F0000) == 0x051F0000 && ((p >> 12) & 0xF) == rx)
        {
            u32 imm = p & 0xFFF;
            u32 lit = arm9Start + (i - 1) * 4 + 8 + ((p & (1 << 23)) ? imm : -imm);
            if (lit - arm9Start < arm9Size && sRegion2LiteralCount < SHADOW_MAX_REGION2)
                sRegion2Literal[sRegion2LiteralCount++] = lit;
            else
                unknown = true;
        }
        else if (code[i + 1] == 0xE12FFF1E && sRegion2McrCount < SHADOW_MAX_REGION2)
            sRegion2Mcr[sRegion2McrCount++] = arm9Start + i * 4;
        else
            unknown = true;
    }
    if (unknown || sRegion2LiteralCount == 0)
    {
        LOG_DEBUG("Shadow: region 2 setup not as expected (%u literals, %u setters)\n",
            sRegion2LiteralCount, sRegion2McrCount);
        return;
    }
    sRegion2Ok = true;

    // The stores. One block, at the initial addresses: code in the autoloads
    // still runs pc-relative within its own block.
    shadow_scan::Block block = { (const uint8_t*)arm9Start, arm9Start, arm9Size };
    static u32 found[SHADOW_MAX_SITES];
    shadow_scan::Scanner scanner(&block, 1, found, SHADOW_MAX_SITES);
    scanner.Scan(0, 1);
    for (u32 i = 0; i < scanner.SiteCount(); i++)
    {
        u32 site = found[i];
        if (site & 1)
        {
            sThumbSites++; // not yet
            continue;
        }
        // the secure area's free space holds the loader's own patches
        if (site - arm9Start < 0x800)
            continue;
        if (usesHighRegs(*(const u32*)site))
        {
            sOddRegs++;
            continue;
        }
        sFinal[sSiteCount] = autoload ? autoload->AdjustInitialToFinal(site) : site;
        sSites[sSiteCount++] = site;
    }
    LOG_DEBUG("Shadow: %u arm sites, %u thumb and %u with sp/lr left out, %u over the limit\n",
        sSiteCount, sThumbSites, sOddRegs, scanner.Overflow());
}

u32 ShadowInstall::SpaceNeeded()
{
    if (!sRegion2Ok || sSiteCount == 0)
        return 0;
    u32 code = (SECTION_SIZE(patch_shadow) + 31) & ~31u;
    return code + sSiteCount * SHADOW_STUB_SIZE;
}

bool ShadowInstall::Install(void* space, u32 runAddress, u32 size)
{
    if (!space || !runAddress || !sRegion2Ok || sSiteCount == 0)
        return false;
    u32 codeSize = (SECTION_SIZE(patch_shadow) + 31) & ~31u;
    if (size < codeSize + SHADOW_STUB_SIZE)
        return false;
    u32 maxStubs = (size - codeSize) / SHADOW_STUB_SIZE;
    u32 count = sSiteCount < maxStubs ? sSiteCount : maxStubs;

    memset(space, 0, size);
    memcpy(space, SECTION_START(patch_shadow), SECTION_SIZE(patch_shadow));
    u32* words = (u32*)space;
    u32 record = runAddress + ((u32)&shadow_record - (u32)SECTION_START(patch_shadow));

    u32 made = 0;
    for (u32 i = 0; i < count; i++)
    {
        u32 initial = sSites[i];
        u32 site = sFinal[i];
        u32 op = *(u32*)initial;
        u32 stubOffset = codeSize + made * SHADOW_STUB_SIZE;
        u32 stubRun = runAddress + stubOffset;
        u32* stub = (u32*)((u8*)space + stubOffset);
        s32 toStub = (s32)(stubRun - (site + 8));
        s32 toRecord = (s32)(record - (stubRun + 8 + 8));
        if (toStub < -0x2000000 || toStub >= 0x2000000 || toRecord < -0x2000000 || toRecord >= 0x2000000)
            continue;
        stub[0] = op;
        stub[1] = 0xE52DE008; // str lr, [sp, #-8]!
        stub[2] = 0xEB000000 | (((u32)toRecord >> 2) & 0xFFFFFF); // bl shadow_record
        stub[3] = site + 4;
        *(u32*)initial = (op & 0xF0000000) | 0x0A000000 | (((u32)toStub >> 2) & 0xFFFFFF);
        made++;
    }
    words[SHADOW_COUNT_FIELD / 4] = made;
    words[SHADOW_STUBS_OFFSET_FIELD / 4] = codeSize;

    // region 2 off: the space is in the arm7's corner the sdk guards from the arm9
    for (u32 i = 0; i < sRegion2LiteralCount; i++)
        *(u32*)sRegion2Literal[i] = 0;
    for (u32 i = 0; i < sRegion2McrCount; i++)
        *(u32*)sRegion2Mcr[i] = 0xE1A00000; // mov r0, r0
    LOG_DEBUG("Shadow at 0x%x: %u stubs of %u sites\n", runAddress, made, sSiteCount);
    return made != 0;
}
