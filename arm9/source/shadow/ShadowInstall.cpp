#include "common.h"
#include <string.h>
#include "sections.h"
#include "IAutoloadAdjuster.h"
#include "ShadowScan.h"
#include "ShadowInstall.h"
#include "cache.h"

DEFINE_SECTION_SYMBOLS(patch_shadow);
extern "C" void shadow_record(void);
extern "C" void shadow_record_thumb(void);
extern "C" void shadow_copied(void);
extern "C" void shadow_filled8(void);
extern "C" void shadow_filled16(void);
extern "C" void shadow_filled32(void);
extern "C" void shadow_overlay_loaded(void);
extern "C" void shadow_vblank(void);
extern "C" u32 shadow_groups;
extern "C" u32 shadow_group_count;

#define SHADOW_STUBS_OFFSET_FIELD   0x0C
#define SHADOW_COUNT_FIELD          0x08
#define SHADOW_STUB_SIZE            16
#define SHADOW_MAX_SITES            512
#define SHADOW_MAX_COPIES           64
#define SHADOW_MAX_OVERLAYS         128
#define SHADOW_MAX_REGION2          8

using shadow_scan::CopySite;

// What gets a stub. Words all: the loader runs from vram, which takes no byte writes.
enum : u32 { ENTRY_ARM = 0, ENTRY_THUMB = 1, ENTRY_THUMB2 = 2, ENTRY_COPY = 3, ENTRY_THUMB_PREV = 4 };
struct Entry
{
    u32 type;
    u32 initial;   // where the loader patches
    u32 final;     // where it runs
    u32 callee;    // copies: the routine's final address, bit 0 for thumb
    u32 link;
    u32 kind;
    u32 overlay;   // 0: the static arm9; else the overlay's id + 1, patched as it loads
    u32 orig;      // the word at initial (two halfwords for thumb)
};

static Entry sEntries[SHADOW_MAX_SITES];
static u32 sEntryCount;
static u32 sRegion2Literal[SHADOW_MAX_REGION2]; // literal words to clear
static u32 sRegion2LiteralCount;
static u32 sRegion2Mcr[SHADOW_MAX_REGION2];     // setter mcr instructions to drop
static u32 sRegion2McrCount;
static u32 sRegion2Ok;
static u32 sArm9Start, sArm9Size;
static u32 sAutoloadFrom; // the static arm9's autoloads, at their initial place, start here
static u32 sStaticFromOverlays;
static u32 sBlockStart, sBlockSize; // what the thumb checks may read: the static arm9 or an overlay
static const u32* sUncompress;
static u32* sOverlayHookTarget;
static u32 sOverlayEntries;
static u32 sOverlayBase; // sEntryCount before the overlays: what the cache holds comes after
static u32 sOverlaysWithSites;
static u32 sVblankRoutine;

// The loader runs with the data cache off. Reading a whole overlay through it
// uncached (the game's decompressor, then the scan) is what took the time, so
// these windows turn it on for main memory, and clean and invalidate it all
// before turning it off again: nothing else reads that memory meanwhile.
__attribute__((target("arm"), noinline)) static u32 dcacheOn()
{
    u32 c;
    asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(c));
    dc_invalidateAll();
    asm volatile("mcr p15, 0, %0, c1, c0, 0" :: "r"(c | 4) : "memory");
    return c;
}

__attribute__((target("arm"), noinline)) static void dcacheOff(u32 c)
{
    dc_flushAll();
    dc_drainWriteBuffer();
    asm volatile("mcr p15, 0, %0, c1, c0, 0" :: "r"(c) : "memory");
    dc_invalidateAll();
}

// The recorder reads the store's registers from r0-r12 as it saved them, and lr
// from the stub's copy; never sp or pc.
static bool usesHighRegs(u32 op)
{
    if ((op & 0x0E000000) == 0x08000000)
        return ((op >> 16) & 0xF) >= 13 || (op & 0xE000);
    u32 rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF;
    bool dual = (op & 0x0E000000) == 0 && (op & 0x60) == 0x60; // strd: rd and rd + 1
    return rn == 13 || rn == 15 || rd == 13 || rd == 15 || (dual && rd >= 12);
}

static inline u32 half(u32 a) { return *(const u16*)a; }
static bool inArm9(u32 a, u32 size) { return a - sBlockStart < sBlockSize && a + size - sBlockStart <= sBlockSize; }

// A thumb instruction the stub can run in its place: no pc, sp or lr, no branch.
static bool thumbMovable(u32 h)
{
    u32 top = h >> 11;
    if (top == 0x09 || (top >= 0x12 && top <= 0x17) || top >= 0x1A)
        return false; // pc-relative ldr, sp-relative, adr/add sp, push/pop/misc, branches, bl
    if ((h & 0xFC00) == 0x4400)
    {
        u32 rm = (h >> 3) & 0xF, d = (h & 7) | ((h >> 4) & 8);
        if (rm >= 13 || d >= 13 || ((h >> 8) & 3) == 3)
            return false; // hi register ops on sp, lr, pc; bx/blx
    }
    return true;
}

// Is lr dead at a thumb site, so that a bl may go there? The routine pushed it
// on the way in and doesn't read it again before it pops pc.
static bool thumbLrDead(u32 site)
{
    bool pushed = false;
    for (u32 a = site - 2; a + 0x400 > site && inArm9(a, 2); a -= 2)
    {
        u32 h = half(a);
        if ((h & 0xFF00) == 0xB500) { pushed = true; break; } // push {.., lr}
        if (h == 0x4770) return false; // bx lr: another routine
        // (a pop {.., pc} may be an early return; a leaf routine shows below,
        // as it gets back with bx lr)
    }
    if (!pushed)
        return false;
    for (u32 a = site + 2; a < site + 0x200 && inArm9(a, 2); a += 2)
    {
        u32 h = half(a);
        if ((h & 0xFF00) == 0xBD00) return true;
        if ((h & 0xF800) == 0xF000) return true; // a call: lr gets a new value
        if ((h & 0xFC00) == 0x4400 && ((h >> 3) & 0xF) == 14) return false; // reads lr
    }
    return false;
}

// Does any branch near a thumb site land on the halfword after it?
static bool thumbBranchedTo(u32 target)
{
    for (u32 a = target - 0x800; a < target + 0x800; a += 2)
    {
        if (!inArm9(a, 2))
            continue;
        u32 h = half(a);
        if ((h & 0xF000) == 0xD000 && (h & 0x0F00) < 0x0E00 && a + 4 + ((s32)(h << 24) >> 23) == target)
            return true;
        if ((h & 0xF800) == 0xE000 && a + 4 + ((s32)(h << 21) >> 20) == target)
            return true;
    }
    return false;
}

static void ScanBlocks(const shadow_scan::Block* blocks, u32 blockCount, u32 overlay, const IAutoloadAdjuster* autoload);

static bool isThumbSite(const u32* sites, u32 n, u32 a)
{
    for (u32 i = 0; i < n; i++)
        if (sites[i] == (a | 1))
            return true;
    return false;
}

void ShadowInstall::Collect(u32 arm9Start, u32 arm9Size, const IAutoloadAdjuster* autoload)
{
    sEntryCount = 0;
    sRegion2LiteralCount = 0;
    sRegion2McrCount = 0;
    sRegion2Ok = 0;
    sArm9Start = arm9Start;
    sArm9Size = arm9Size;
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
    sRegion2Ok = 1;

    sBlockStart = arm9Start;
    sBlockSize = arm9Size;
    shadow_scan::Block block = { (const uint8_t*)arm9Start, arm9Start, arm9Size };
    u32 before = sEntryCount;
    // where the autoloads start (initial place), for sites found later
    sAutoloadFrom = arm9Start + arm9Size;
    if (autoload)
        while (sAutoloadFrom - 4 > arm9Start && autoload->AdjustInitialToFinal(sAutoloadFrom - 4) != sAutoloadFrom - 4)
            sAutoloadFrom -= 4;
    dc_flushAll(); // the loader's patches, written uncached, are in memory already
    u32 cache = dcacheOn();
    ScanBlocks(&block, 1, 0, autoload);
    dcacheOff(cache);
    LOG_DEBUG("Shadow: %u stubs for the static arm9\n", sEntryCount - before);
}

// A static arm9 arm site found by an overlay's scan, once.
static void addStaticArm(u32 site)
{
    if (sEntryCount >= SHADOW_MAX_SITES || usesHighRegs(*(const u32*)site))
        return;
    for (u32 i = 0; i < sEntryCount; i++)
        if (!sEntries[i].overlay && sEntries[i].initial == site)
            return;
    Entry& e = sEntries[sEntryCount++];
    e.type = ENTRY_ARM;
    e.overlay = 0;
    e.initial = site;
    e.orig = *(const u32*)site;
    e.final = site;
    sStaticFromOverlays++;
}

// The stores and copies in blocks[0] (calls may go into the others), as entries.
static void ScanBlocks(const shadow_scan::Block* blocks, u32 blockCount, u32 overlay, const IAutoloadAdjuster* autoload)
{
    static u32 found[SHADOW_MAX_SITES];
    static CopySite copies[SHADOW_MAX_COPIES];
    shadow_scan::Scanner scanner(blocks, blockCount, found, SHADOW_MAX_SITES);
    scanner.SetCopyOutput(copies, SHADOW_MAX_COPIES);
    scanner.Scan(0, 1);
    u32 n = scanner.SiteCount();
    u32 lo = blocks[0].runAddress;
    // the static arm9's secure area holds the loader's own patches
    u32 skip = overlay ? 0 : 0x800;
    auto adjust = [&](u32 a) { return autoload ? autoload->AdjustInitialToFinal(a) : a; };

    // thumb sites in address order: a store right after another goes in the same stub
    for (u32 i = 1; i < n; i++)
        for (u32 j = i; j > 0 && found[j - 1] > found[j]; j--)
        {
            u32 t = found[j]; found[j] = found[j - 1]; found[j - 1] = t;
        }

    u32 arm = 0, thumb = 0, thumbOut = 0, oddRegs = 0, consumedEnd = 0;
    for (u32 i = 0; i < n && sEntryCount < SHADOW_MAX_SITES; i++)
    {
        u32 site = found[i] & ~1u;
        if (overlay && blockCount > 1 && !(found[i] & 1) && site - sArm9Start >= 0x800 && site < sAutoloadFrom)
        {
            // an overlay's call into a static setter with an io base: a static
            // site the static pass can't see (its callers aren't there), patched
            // in place like the others
            addStaticArm(site);
            continue;
        }
        if (site - lo < skip || site - lo >= blocks[0].size)
            continue;
        Entry& e = sEntries[sEntryCount];
        e.overlay = overlay;
        if (found[i] & 1)
        {
            if (site < consumedEnd)
                continue; // the second store of the one before
            u32 next = site + 2;
            bool two = isThumbSite(found, n, next);
            if (inArm9(site, 4) && thumbLrDead(site) && !thumbBranchedTo(next)
                && (two || thumbMovable(half(next))))
            {
                e.type = two ? ENTRY_THUMB2 : ENTRY_THUMB;
                e.initial = site;
                consumedEnd = site + 4;
            }
            else if (site - 2 >= consumedEnd && site - 2 - lo >= skip && inArm9(site - 2, 4) && thumbLrDead(site - 2)
                && !thumbBranchedTo(site) && thumbMovable(half(site - 2)))
            {
                // the instruction before it goes in the stub instead
                e.type = ENTRY_THUMB_PREV;
                e.initial = site - 2;
                consumedEnd = site + 2;
            }
            else
            {
                thumbOut++;
                continue;
            }
            e.orig = half(e.initial) | (half(e.initial + 2) << 16);
            e.final = adjust(e.initial);
            sEntryCount++;
            thumb++;
            continue;
        }
        if (usesHighRegs(*(const u32*)site))
        {
            oddRegs++;
            continue;
        }
        e.type = ENTRY_ARM;
        e.initial = site;
        e.orig = *(const u32*)site;
        e.final = adjust(site);
        sEntryCount++;
        arm++;
    }
    u32 copyCount = 0;
    for (u32 i = 0; i < scanner.CopyCount() && sEntryCount < SHADOW_MAX_SITES; i++)
    {
        const CopySite& c = copies[i];
        u32 at = c.at & ~1u;
        if (at - lo < skip || !inArm9(at, 4))
            continue;
        if (c.link == shadow_scan::LINK_THUMB_CALL ? !(c.at & 1) : (c.at & 1))
            continue;
        Entry& e = sEntries[sEntryCount++];
        e.type = ENTRY_COPY;
        e.overlay = overlay;
        e.initial = at;
        e.orig = half(at) | (half(at + 2) << 16);
        e.final = adjust(at);
        e.callee = adjust(c.callee & ~1u) | (c.callee & 1);
        e.link = c.link;
        e.kind = c.kind;
        copyCount++;
    }
    if (overlay == 0 || arm + thumb + copyCount)
        LOG_DEBUG("Shadow%s: %u arm sites, %u thumb (%u left out), %u copies; %u with sp/lr left out, %u over the limit\n",
            overlay ? " overlay" : "", arm, thumb, thumbOut, copyCount, oddRegs, scanner.Overflow());
}

void ShadowInstall::SetUncompress(const u32* uncompress)
{
    sUncompress = uncompress;
}

void ShadowInstall::SetOverlayHookTarget(u32* target)
{
    sOverlayHookTarget = target;
}

bool ShadowInstall::WantsOverlays()
{
    sOverlayEntries = 0;
    sOverlaysWithSites = 0;
    sStaticFromOverlays = 0;
    sOverlayBase = sEntryCount;
    return sRegion2Ok; // through the FS_StartOverlay hook, or else from vblank
}

typedef void (*uncompress_func_t)(void* compressedEnd);

void ShadowInstall::ScanOverlay(const u32* entry, u32 fileSize)
{
    u32 id = entry[0], ram = entry[1], ramSize = entry[2], compressed = entry[7];
    dc_flushAll();
    u32 cache = dcacheOn();
    if ((compressed >> 24) & 1)
    {
        u32 packed = compressed & 0xFFFFFF;
        if (!sUncompress || packed == 0 || packed > fileSize)
            goto clear;
        u32 end = ram + packed;
        // the footer: the uncompressed size grows the end by the word before it
        if (*(u32*)(end - 4) + packed > ramSize)
            goto clear;
        ((uncompress_func_t)sUncompress)((void*)end);
    }
    if (sRegion2Ok && sEntryCount < SHADOW_MAX_SITES)
    {
        sBlockStart = ram;
        sBlockSize = ramSize;
        shadow_scan::Block blocks[2] = {
            { (const uint8_t*)ram, ram, ramSize },
            { (const uint8_t*)sArm9Start, sArm9Start, sArm9Size } };
        u32 before = sEntryCount;
        ScanBlocks(blocks, 2, id + 1, nullptr);
        if (sEntryCount != before)
            sOverlaysWithSites++;
        sOverlayEntries += sEntryCount - before;
    }
clear:
    memset((void*)ram, 0, ramSize);
    dcacheOff(cache);
}

// The overlays' scan result, cached on the card by the arm7: what the overlay
// pass added to sEntries, after three counters and the entry count. It holds
// only for this loader (its code) and this static arm9 scan result.
extern "C" u8 __text_start[], __text_end[];
static_assert(sizeof(Entry) == 32, "the cache writes entries as 8 words");

u32 ShadowInstall::CacheKey()
{
    if (!sRegion2Ok)
        return 0;
    u32 h = 2166136261u;
    auto mix = [&](u32 w) { h = (h ^ w) * 16777619u; };
    for (const u32* p = (const u32*)__text_start; p < (const u32*)__text_end; p++)
        mix(*p);
    mix(SHADOW_MAX_SITES);
    mix((u32)sUncompress);
    mix(sArm9Start);
    mix(sArm9Size);
    mix(sAutoloadFrom);
    mix(sOverlayBase);
    for (u32 i = 0; i < sOverlayBase; i++)
        for (u32 k = 0; k < 8; k++)
            mix(((const u32*)&sEntries[i])[k]);
    return h | 1;
}

u32 ShadowInstall::CacheWords()
{
    return 4 + (sEntryCount - sOverlayBase) * 8;
}

u32 ShadowInstall::CacheWord(u32 i)
{
    switch (i)
    {
        case 0: return sStaticFromOverlays;
        case 1: return sOverlayEntries;
        case 2: return sOverlaysWithSites;
        case 3: return sEntryCount - sOverlayBase;
    }
    i -= 4;
    return ((const u32*)&sEntries[sOverlayBase + i / 8])[i % 8];
}

bool ShadowInstall::CacheLoad(u32 words, u32 (*next)())
{
    u32 head[4] = { 0, 0, 0, 0 };
    u32 count = 0;
    bool ok = sRegion2Ok && words >= 4;
    for (u32 i = 0; i < words; i++)
    {
        u32 w = next();
        if (!ok)
            continue;
        if (i < 4)
        {
            head[i] = w;
            if (i == 3)
            {
                count = w;
                ok = words == 4 + count * 8 && count <= SHADOW_MAX_SITES - sOverlayBase;
            }
            continue;
        }
        ((u32*)&sEntries[sOverlayBase])[i - 4] = w;
    }
    if (!ok)
        return false;
    sEntryCount = sOverlayBase + count;
    sStaticFromOverlays = head[0];
    sOverlayEntries = head[1];
    sOverlaysWithSites = head[2];
    LOG_DEBUG("Shadow: overlay scan from the cache, %u entries\n", count);
    return true;
}

// After the stubs: the overlays' patches, a group (id, the patches, how many) per
// overlay, then two halfword patches (address, find | put << 16) an entry.
static u32 extraFor(u32 entries)
{
    u32 overlayEntries = 0, groups = 0, last = 0;
    for (u32 i = 0; i < entries; i++)
    {
        if (!sEntries[i].overlay)
            continue;
        overlayEntries++;
        if (sEntries[i].overlay != last)
            groups++;
        last = sEntries[i].overlay;
    }
    return groups * 12 + overlayEntries * 16;
}

u32 ShadowInstall::VblankRoutine()
{
    return sVblankRoutine;
}

u32 ShadowInstall::SpaceNeeded()
{
    if (!sRegion2Ok || sEntryCount == 0)
        return 0;
    u32 code = (SECTION_SIZE(patch_shadow) + 31) & ~31u;
    return code + sEntryCount * SHADOW_STUB_SIZE + extraFor(sEntryCount);
}

static bool armBranch(u32 from, u32 to, u32& field)
{
    s32 d = (s32)(to - (from + 8));
    if (d < -0x2000000 || d >= 0x2000000)
        return false;
    field = ((u32)d >> 2) & 0xFFFFFF;
    return true;
}

// thumb bl (to thumb) or blx (to arm, word aligned) at from
static bool thumbCall(u32 from, u32 to, bool toArm, u16& hi, u16& lo)
{
    u32 base = toArm ? (from + 4) & ~3u : from + 4;
    s32 d = (s32)(to - base);
    if (d < -0x400000 || d >= 0x400000)
        return false;
    hi = 0xF000 | (((u32)d >> 12) & 0x7FF);
    lo = (toArm ? 0xE800 : 0xF800) | (((u32)d >> 1) & 0x7FF);
    return true;
}

bool ShadowInstall::Install(void* space, u32 runAddress, u32 size)
{
    if (!space || !runAddress || !sRegion2Ok || sEntryCount == 0)
        return false;
    u32 codeSize = (SECTION_SIZE(patch_shadow) + 31) & ~31u;
    if (size < codeSize + SHADOW_STUB_SIZE)
        return false;
    u32 count = sEntryCount;
    while (count && codeSize + count * SHADOW_STUB_SIZE + extraFor(count) > size)
        count--;
    if (count == 0)
        return false;

    memset(space, 0, size);
    memcpy(space, SECTION_START(patch_shadow), SECTION_SIZE(patch_shadow));
    u32* words = (u32*)space;
    auto run = [&](const void* f) { return runAddress + ((u32)f - (u32)SECTION_START(patch_shadow)); };
    auto at = [&](const void* f) { return (u32*)((u8*)space + ((u32)f - (u32)SECTION_START(patch_shadow))); };
    u32 record = run((const void*)shadow_record);
    u32 recordThumb = run((const void*)shadow_record_thumb);
    u32 copied[4] = { run((const void*)shadow_copied), run((const void*)shadow_filled8),
        run((const void*)shadow_filled16), run((const void*)shadow_filled32) };

    // the overlays' groups and patches, after the stubs of all entries
    u32 groupsOffset = codeSize + count * SHADOW_STUB_SIZE;
    u32 groupCount = 0, lastOverlay = 0;
    for (u32 i = 0; i < count; i++)
        if (sEntries[i].overlay && sEntries[i].overlay != lastOverlay)
        {
            groupCount++;
            lastOverlay = sEntries[i].overlay;
        }
    u32 patchesOffset = groupsOffset + groupCount * 12;
    u32* groups = (u32*)((u8*)space + groupsOffset);
    u32* patches = (u32*)((u8*)space + patchesOffset);
    u32 groupsMade = 0, patchesMade = 0;
    lastOverlay = 0;

    // the site's new halfwords: in place for the static arm9, as patches for an overlay
    auto put = [&](const Entry& e, u32 value)
    {
        if (!e.overlay)
        {
            u16* h = (u16*)e.initial;
            h[0] = value;
            h[1] = value >> 16;
            return;
        }
        if (e.overlay != lastOverlay)
        {
            groups[groupsMade * 3 + 0] = e.overlay - 1;
            groups[groupsMade * 3 + 1] = runAddress + patchesOffset + patchesMade * 8;
            groups[groupsMade * 3 + 2] = 0;
            groupsMade++;
            lastOverlay = e.overlay;
        }
        for (u32 k = 0; k < 2; k++)
        {
            patches[patchesMade * 2 + 0] = e.final + k * 2;
            patches[patchesMade * 2 + 1] = ((e.orig >> (16 * k)) & 0xFFFF) | (((value >> (16 * k)) & 0xFFFF) << 16);
            patchesMade++;
            groups[(groupsMade - 1) * 3 + 2]++;
        }
    };

    u32 made = 0, failed = 0;
    for (u32 i = 0; i < count; i++)
    {
        const Entry& e = sEntries[i];
        u32 stubOffset = codeSize + made * SHADOW_STUB_SIZE;
        u32 stubRun = runAddress + stubOffset;
        u32* stub = (u32*)((u8*)space + stubOffset);
        bool ok = false;
        if (e.type == ENTRY_ARM)
        {
            u32 op = e.orig;
            u32 toStub, toRecord;
            if (armBranch(e.final, stubRun, toStub) && armBranch(stubRun + 8, record, toRecord))
            {
                stub[0] = op;
                stub[1] = 0xE52DE008; // str lr, [sp, #-8]!
                stub[2] = 0xEB000000 | toRecord; // bl shadow_record
                stub[3] = e.final + 4;
                put(e, (op & 0xF0000000) | 0x0A000000 | toStub);
                ok = true;
            }
        }
        else if (e.type == ENTRY_THUMB || e.type == ENTRY_THUMB2)
        {
            // push {lr} / store / blx record / next (/ blx record) / pop {pc}
            u16 hi, lo, rhi, rlo, r2hi, r2lo;
            if (thumbCall(e.final, stubRun, false, hi, lo)
                && thumbCall(stubRun + 4, recordThumb, true, rhi, rlo)
                && thumbCall(stubRun + 10, recordThumb, true, r2hi, r2lo))
            {
                u32 store = e.orig & 0xFFFF, next = e.orig >> 16;
                stub[0] = 0xB500 | (store << 16);
                stub[1] = rhi | (rlo << 16);
                if (e.type == ENTRY_THUMB)
                {
                    stub[2] = next | (0xBD00 << 16);
                    stub[3] = 0;
                }
                else
                {
                    stub[2] = next | (r2hi << 16);
                    stub[3] = r2lo | (0xBD00 << 16);
                }
                put(e, hi | (lo << 16));
                ok = true;
            }
        }
        else if (e.type == ENTRY_THUMB_PREV)
        {
            // push {lr} / prev / store / blx record / pop {pc}
            u16 hi, lo, rhi, rlo;
            if (thumbCall(e.final, stubRun, false, hi, lo) && thumbCall(stubRun + 6, recordThumb, true, rhi, rlo))
            {
                u32 prev = e.orig & 0xFFFF, store = e.orig >> 16;
                stub[0] = 0xB500 | (prev << 16);
                stub[1] = store | (rhi << 16);
                stub[2] = rlo | (0xBD00 << 16);
                stub[3] = 0;
                put(e, hi | (lo << 16));
                ok = true;
            }
        }
        else if (e.type == ENTRY_COPY)
        {
            // stmfd sp!, {r0-r2, lr} / bl(x) routine / bl shadow_copied_<kind> / the call
            u32 callee = e.callee & ~1u;
            bool calleeThumb = e.callee & 1;
            u32 toCallee, toCopied;
            if (armBranch(stubRun + 4, callee, toCallee) && e.kind < 4 && armBranch(stubRun + 8, copied[e.kind], toCopied))
            {
                stub[0] = 0xE92D4007;
                stub[1] = calleeThumb
                    ? 0xFA000000 | (((callee - (stubRun + 12)) & 2) << 23) | toCallee // blx: H for the halfword
                    : 0xEB000000 | toCallee;
                stub[2] = 0xEB000000 | toCopied;
                stub[3] = e.final;
                if (e.link == shadow_scan::LINK_THUMB_CALL)
                {
                    u16 hi, lo;
                    if (thumbCall(e.final, stubRun, true, hi, lo))
                    {
                        put(e, hi | (lo << 16));
                        ok = true;
                    }
                }
                else
                {
                    u32 op = e.orig;
                    u32 toStub;
                    if (armBranch(e.final, stubRun, toStub))
                    {
                        u32 cond = (op >> 28) == 0xF ? 0xE0000000 : op & 0xF0000000; // blx imm: always
                        bool call = e.link == shadow_scan::LINK_ARM_CALL;
                        put(e, cond | (call ? 0x0B000000 : 0x0A000000) | toStub);
                        ok = true;
                    }
                }
            }
        }
        if (ok)
            made++;
        else
        {
            failed++;
            memset(stub, 0, SHADOW_STUB_SIZE);
        }
    }
    words[SHADOW_COUNT_FIELD / 4] = made;
    words[SHADOW_STUBS_OFFSET_FIELD / 4] = codeSize;
    *at(&shadow_groups) = runAddress + groupsOffset;
    *at(&shadow_group_count) = groupsMade;
    sVblankRoutine = 0;
    if (groupsMade && sOverlayHookTarget)
        *sOverlayHookTarget = run((const void*)shadow_overlay_loaded);
    else if (groupsMade)
        sVblankRoutine = run((const void*)shadow_vblank);

    // region 2 off: the space is in the arm7's corner the sdk guards from the arm9
    for (u32 i = 0; i < sRegion2LiteralCount; i++)
        *(u32*)sRegion2Literal[i] = 0;
    for (u32 i = 0; i < sRegion2McrCount; i++)
        *(u32*)sRegion2Mcr[i] = 0xE1A00000; // mov r0, r0
    LOG_DEBUG("Shadow at 0x%x: %u stubs of %u (%u out of reach), %u overlays with %u patches, %u static sites from overlay calls\n",
        runAddress, made, sEntryCount, failed, groupsMade, patchesMade, sStaticFromOverlays);
    return made != 0;
}
