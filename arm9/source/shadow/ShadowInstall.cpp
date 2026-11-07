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
#define SHADOW_GROUP_SIZE           32
#define SHADOW_MAX_SITES            768
#define SHADOW_MAX_COPIES           64
#define SHADOW_MAX_OVERLAYS         128
#define SHADOW_MAX_REGION2          8

using shadow_scan::CopySite;

// What gets a stub. Words and halfwords only: the loader runs from vram, which
// takes no byte writes.
enum : u32 { ENTRY_ARM = 0, ENTRY_THUMB = 1, ENTRY_THUMB2 = 2, ENTRY_COPY = 3, ENTRY_THUMB_PREV = 4 };
struct Entry
{
    u32 initial;   // where the loader patches
    u32 final;     // where it runs
    u32 callee;    // copies: the routine's final address, bit 0 for thumb
    u32 orig;      // the word at initial (two halfwords for thumb)
    u16 overlay;   // 0: the static arm9; else the overlay's id + 1, patched as it loads
    u16 type;
    u16 link;
    u16 kind;
};

// The overlays with sites: where each goes, for the stub slots they share.
struct OverlaySpan
{
    u32 overlay;   // id + 1
    u32 ram;
    u32 size;
};
static OverlaySpan sSpans[SHADOW_MAX_OVERLAYS];
static u32 sSpanCount;

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

// ldr rx, [pc, #imm]: movable into a stub ahead of the store only, with its
// literal in the stub's last word (as ldr rx, [pc, #8] from the stub's second halfword)
static bool thumbLiteralLoad(u32 h)
{
    return (h >> 11) == 0x09;
}

static u32 thumbLiteralOf(u32 at)
{
    u32 h = half(at);
    return *(const u32*)(((at + 4) & ~3u) + (h & 0xFF) * 4);
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
        if ((h & 0xFF87) == 0x4700)
        {
            // bx rN: a return only as pop {rN} / bx rN (the routine's own push gave it). Any
            // other is a tail call or a jump that still needs lr: Mario Kart DS has
            // ldr r0, =io / str r1, [r0] / ldr r3, =routine / bx r3 in a routine that never
            // pushed lr, and the backward look above found the push of the routine before it.
            u32 rn = (h >> 3) & 0xF;
            u32 prev = half(a - 2);
            return rn < 8 && (prev & 0xFF00) == 0xBC00 && (prev & (1u << rn));
        }
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
                && !thumbBranchedTo(site) && (thumbMovable(half(site - 2))
                    || (thumbLiteralLoad(half(site - 2)) && inArm9(((site + 2) & ~3u) + (half(site - 2) & 0xFF) * 4, 4))))
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
    sSpanCount = 0;
    return sRegion2Ok; // through the FS_StartOverlay hook, or else from vblank
}

typedef void (*uncompress_func_t)(void* compressedEnd);
static u32 sOverlaysScanned, sOverlaysPacked; // for the log: seen, and left compressed
static u32 sOverlaysLz40; // unpacked here, packed by the game itself
static u32 windowOf(const Entry& e);

// LZ40, which some games use to pack their own overlays (Golden Sun Dark Dawn):
// 0x40 and the unpacked size in 24 bits, then flag bytes whose negation marks,
// from the top bit, which of the next eight items are copies. A copy is a
// halfword: a 12-bit distance over a 4-bit count, where 0 and 1 take the count
// from the next byte (+0x10) or halfword (+0x110). Unpacked in place: the file
// moves to the end of the overlay, and its last bytes wait on the stack, as the
// output can run a little over the input it hasn't read yet near the end.
static bool unpackLz40(u8* ram, u32 ramSize, u32 fileSize)
{
    if (fileSize < 5 || ram[0] != 0x40 || (u32)(ram[1] | ram[2] << 8 | ram[3] << 16) != ramSize)
        return false;
    constexpr u32 TAIL = 256;
    u8 tail[TAIL];
    u32 tailFrom = fileSize > TAIL ? fileSize - TAIL : 0;
    memcpy(tail, ram + tailFrom, fileSize - tailFrom);
    u8* in = ram + ramSize - fileSize;
    memmove(in, ram, fileSize);
    auto rd = [&](u32 i) -> u32 { return i >= tailFrom ? tail[i - tailFrom] : in[i]; };
    u32 p = 4, o = 0;
    while (o < ramSize)
    {
        if (p >= fileSize)
            return false;
        u32 bits = -rd(p++);
        for (u32 k = 0; k < 8 && o < ramSize; k++, bits <<= 1)
        {
            u32 n = 1, disp = 0;
            if (bits & 0x80)
            {
                if (p + 2 > fileSize)
                    return false;
                u32 v = rd(p) | rd(p + 1) << 8;
                p += 2;
                disp = v >> 4;
                n = v & 0xF;
                if (n == 0)
                {
                    if (p + 1 > fileSize)
                        return false;
                    n = rd(p++) + 0x10;
                }
                else if (n == 1)
                {
                    if (p + 2 > fileSize)
                        return false;
                    n = (rd(p) | rd(p + 1) << 8) + 0x110;
                    p += 2;
                }
                if (disp == 0 || disp > o || n > ramSize - o)
                    return false;
            }
            else if (p >= fileSize)
                return false;
            // the input not yet read and not on the stack must stay whole
            if (p < tailFrom && ram + o + n > in + p)
                return false;
            if (disp == 0)
                ram[o++] = rd(p++);
            else
                for (u32 end = o + n; o < end; o++)
                    ram[o] = ram[o - disp];
        }
    }
    return true;
}

void ShadowInstall::ScanOverlay(const u32* entry, u32 fileSize)
{
    u32 id = entry[0], ram = entry[1], ramSize = entry[2], compressed = entry[7];
    sOverlaysScanned++;
    dc_flushAll();
    u32 cache = dcacheOn();
    if ((compressed >> 24) & 1)
    {
        u32 packed = compressed & 0xFFFFFF;
        if (!sUncompress || packed == 0 || packed > fileSize)
        {
            sOverlaysPacked++;
            goto clear;
        }
        u32 end = ram + packed;
        // the footer: the uncompressed size grows the end by the word before it
        if (*(u32*)(end - 4) + packed > ramSize)
        {
            sOverlaysPacked++;
            goto clear;
        }
        ((uncompress_func_t)sUncompress)((void*)end);
    }
    else if (fileSize < ramSize && *(const u8*)ram == 0x40)
    {
        if (!unpackLz40((u8*)ram, ramSize, fileSize))
        {
            sOverlaysPacked++;
            goto clear;
        }
        sOverlaysLz40++;
    }
    if (sRegion2Ok && sEntryCount < SHADOW_MAX_SITES && sSpanCount < SHADOW_MAX_OVERLAYS)
    {
        sBlockStart = ram;
        sBlockSize = ramSize;
        shadow_scan::Block blocks[2] = {
            { (const uint8_t*)ram, ram, ramSize },
            { (const uint8_t*)sArm9Start, sArm9Start, sArm9Size } };
        u32 before = sEntryCount;
        ScanBlocks(blocks, 2, id + 1, nullptr);
        if (sEntryCount != before)
        {
            sOverlaysWithSites++;
            if (sSpanCount < SHADOW_MAX_OVERLAYS)
                sSpans[sSpanCount++] = { id + 1, ram, ramSize };
            // its windows' entries together, a group each (stable, few entries)
            for (u32 i = before + 1; i < sEntryCount; i++)
            {
                Entry e = sEntries[i];
                u32 w = windowOf(e), j = i;
                for (; j > before && windowOf(sEntries[j - 1]) > w; j--)
                    sEntries[j] = sEntries[j - 1];
                sEntries[j] = e;
            }
        }
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
static_assert(sizeof(Entry) == 24, "the cache writes entries as 6 words");
#define ENTRY_WORDS (sizeof(Entry) / 4)

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
        for (u32 k = 0; k < ENTRY_WORDS; k++)
            mix(((const u32*)&sEntries[i])[k]);
    return h | 1;
}

// Words: the three counters, the entry count, the span count, the entries, the spans.
u32 ShadowInstall::CacheWords()
{
    return 5 + (sEntryCount - sOverlayBase) * ENTRY_WORDS + sSpanCount * 3;
}

u32 ShadowInstall::CacheWord(u32 i)
{
    switch (i)
    {
        case 0: return sStaticFromOverlays;
        case 1: return sOverlayEntries;
        case 2: return sOverlaysWithSites;
        case 3: return sEntryCount - sOverlayBase;
        case 4: return sSpanCount;
    }
    i -= 5;
    u32 entryWords = (sEntryCount - sOverlayBase) * ENTRY_WORDS;
    if (i < entryWords)
        return ((const u32*)&sEntries[sOverlayBase])[i];
    i -= entryWords;
    return ((const u32*)sSpans)[i];
}

bool ShadowInstall::CacheLoad(u32 words, u32 (*next)())
{
    u32 head[5] = { 0, 0, 0, 0, 0 };
    u32 count = 0, spans = 0;
    bool ok = sRegion2Ok && words >= 5;
    for (u32 i = 0; i < words; i++)
    {
        u32 w = next();
        if (!ok)
            continue;
        if (i < 5)
        {
            head[i] = w;
            if (i == 4)
            {
                count = head[3];
                spans = w;
                ok = count <= SHADOW_MAX_SITES - sOverlayBase && spans <= SHADOW_MAX_OVERLAYS
                    && words == 5 + count * ENTRY_WORDS + spans * 3;
            }
            continue;
        }
        u32 k = i - 5;
        if (k < count * ENTRY_WORDS)
            ((u32*)&sEntries[sOverlayBase])[k] = w;
        else
            ((u32*)sSpans)[k - count * ENTRY_WORDS] = w;
    }
    if (!ok)
        return false;
    sEntryCount = sOverlayBase + count;
    sSpanCount = spans;
    sStaticFromOverlays = head[0];
    sOverlayEntries = head[1];
    sOverlaysWithSites = head[2];
    LOG_DEBUG("Shadow: overlay scan from the cache, %u entries\n", count);
    return true;
}

// The space: this code, a fixed stub per static site and per overlay copy call,
// the overlays' shared stub slots, a group per overlay (8 words), then each
// group's records (ShadowPatchCode.s): a halfword a site, then a word and three
// words a copy call.
struct Layout
{
    u32 fixed;        // fixed stubs
    u32 slots;        // shared slots
    u32 groups;
    u32 recordBytes;
    u32 slotStart[SHADOW_MAX_OVERLAYS + 1]; // per group, in slots
    u32 size;         // all of it
};
static Layout sLayout;

// Room elsewhere for the overlays' shared slots (what the arm9 patch heap had
// left, zeros the game never uses), when the space itself is short.
static u32 sExtraSlots, sExtraSlotsSize;

void ShadowInstall::SetExtraSlots(u32 address, u32 size)
{
    sExtraSlots = address;
    sExtraSlotsSize = size;
}

// whether the slots go to the extra room (then the space needs that much less)
static bool slotsElsewhere(const Layout& l, u32 size)
{
    return l.size > size && sExtraSlots && l.slots * SHADOW_STUB_SIZE <= sExtraSlotsSize;
}

static bool fits(const Layout& l, u32 size)
{
    return l.size <= size || (slotsElsewhere(l, size) && l.size - l.slots * SHADOW_STUB_SIZE <= size);
}

// Overlays left out of the space (their entries' overlay values), when it is short.
static u32 sLeftOut[SHADOW_MAX_OVERLAYS];
static u32 sLeftOutCount;

static bool leftOut(u32 overlay)
{
    for (u32 i = 0; i < sLeftOutCount; i++)
        if (sLeftOut[i] == overlay)
            return true;
    return false;
}

static bool spanOf(u32 overlay, u32& ram, u32& size)
{
    for (u32 i = 0; i < sSpanCount; i++)
        if (sSpans[i].overlay == overlay)
        {
            ram = sSpans[i].ram;
            size = sSpans[i].size;
            return true;
        }
    return false;
}

// A group's records are halfwords from its base (words for arm sites, halfwords
// for thumb ones), so an overlay bigger than that reach (Golden Sun Dark Dawn's
// first one, 700 KB) gets a group per window of it.
#define SHADOW_WINDOW_SHIFT 17

static u32 windowOf(const Entry& e)
{
    u32 ram = 0, size = 0;
    if (!e.overlay || !spanOf(e.overlay, ram, size) || e.final < ram)
        return 0;
    return (e.final - ram) >> SHADOW_WINDOW_SHIFT;
}

static u32 groupKey(const Entry& e)
{
    return e.overlay | windowOf(e) << 16;
}

// The layout of the first count entries. Two overlays share a slot only when
// their memory overlaps: then they can't be in at once.
static const Layout& layoutFor(u32 count)
{
    Layout& l = sLayout;
    l.fixed = l.slots = l.groups = l.recordBytes = 0;
    static u32 groupOverlay[SHADOW_MAX_OVERLAYS + 1], groupSlots[SHADOW_MAX_OVERLAYS + 1], groupCopies[SHADOW_MAX_OVERLAYS + 1];
    u32 last = 0;
    for (u32 i = 0; i < count; i++)
    {
        const Entry& e = sEntries[i];
        if (e.overlay && leftOut(e.overlay))
            continue;
        if (!e.overlay || e.type == ENTRY_COPY)
            l.fixed++;
        if (!e.overlay)
            continue;
        u32 key = groupKey(e);
        if (key != last && l.groups <= SHADOW_MAX_OVERLAYS)
        {
            groupOverlay[l.groups] = e.overlay;
            groupSlots[l.groups] = groupCopies[l.groups] = 0;
            l.groups++;
        }
        last = key;
        if (e.type != ENTRY_COPY)
            groupSlots[l.groups - 1]++;
        else
            groupCopies[l.groups - 1]++;
    }
    for (u32 g = 0; g < l.groups; g++)
        l.recordBytes += ((groupSlots[g] * 2 + 3) & ~3u) + 4 + groupCopies[g] * 12;
    for (u32 g = 0; g < l.groups; g++)
    {
        u32 ram = 0, size = 0;
        bool known = spanOf(groupOverlay[g], ram, size);
        u32 n = groupSlots[g];
        // the lowest start where every group already there it would share with overlaps it
        u32 best = l.slots;
        for (u32 c = 0; c <= g; c++)
        {
            u32 start = c == 0 ? 0 : l.slotStart[c - 1] + groupSlots[c - 1];
            if (start >= best)
                continue;
            bool ok = true;
            for (u32 o = 0; o < g && ok; o++)
            {
                if (start >= l.slotStart[o] + groupSlots[o] || l.slotStart[o] >= start + n || n == 0 || groupSlots[o] == 0)
                    continue;
                // the windows of one overlay are in at once
                u32 oram = 0, osize = 0;
                ok = known && groupOverlay[o] != groupOverlay[g] && spanOf(groupOverlay[o], oram, osize)
                    && oram < ram + size && ram < oram + osize;
            }
            if (ok)
                best = start;
        }
        l.slotStart[g] = best;
        if (best + n > l.slots)
            l.slots = best + n;
    }
    u32 code = (SECTION_SIZE(patch_shadow) + 31) & ~31u;
    l.size = code + (l.fixed + l.slots) * SHADOW_STUB_SIZE + l.groups * SHADOW_GROUP_SIZE + l.recordBytes;
    return l;
}

u32 ShadowInstall::VblankRoutine()
{
    return sVblankRoutine;
}

u32 ShadowInstall::SpaceNeeded()
{
    if (!sRegion2Ok || sEntryCount == 0)
        return 0;
    sLeftOutCount = 0;
    return layoutFor(sEntryCount).size;
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
    // Short of space: overlays are left out whole, the one with the most sites
    // first (its records, and often the most slots); then the ones that fit
    // after all go back, the fewest sites first; then the static sites from the end.
    sLeftOutCount = 0;
    u32 count = sEntryCount;
    // the overlays and their site counts (an overlay's entries are together)
    static u32 runOverlay[SHADOW_MAX_OVERLAYS], runSites[SHADOW_MAX_OVERLAYS];
    u32 runs = 0;
    for (u32 i = 0; i < count; i++)
    {
        u32 overlay = sEntries[i].overlay;
        if (!overlay)
            continue;
        if (runs && runOverlay[runs - 1] == overlay)
            runSites[runs - 1]++;
        else if (runs < SHADOW_MAX_OVERLAYS)
        {
            runOverlay[runs] = overlay;
            runSites[runs++] = 1;
        }
    }
    while (!fits(layoutFor(count), size) && sLeftOutCount < SHADOW_MAX_OVERLAYS)
    {
        u32 best = 0, bestSites = 0;
        for (u32 r = 0; r < runs; r++)
            if (runSites[r] > bestSites && !leftOut(runOverlay[r]))
            {
                best = runOverlay[r];
                bestSites = runSites[r];
            }
        if (!best)
            break;
        sLeftOut[sLeftOutCount++] = best;
    }
    // sLeftOut is by sites, most first: back from the end
    for (u32 k = sLeftOutCount; k-- > 0;)
    {
        u32 overlay = sLeftOut[k];
        sLeftOut[k] = 0;
        if (!fits(layoutFor(count), size))
            sLeftOut[k] = overlay;
    }
    for (u32 k = 0; k < sLeftOutCount; k++)
        if (sLeftOut[k])
            LOG_DEBUG("Shadow: overlay %u left out\n", sLeftOut[k] - 1);
    while (count && !fits(layoutFor(count), size))
        count--;
    if (count == 0)
        return false;
    const Layout& l = layoutFor(count);
    const bool elsewhere = slotsElsewhere(l, size);
    const u32 slotBytes = elsewhere ? 0 : l.slots * SHADOW_STUB_SIZE;
    LOG_DEBUG("Shadow: 0x%x of 0x%x bytes\n", l.size - (elsewhere ? l.slots * SHADOW_STUB_SIZE : 0), size);
    if (elsewhere)
    {
        memset((void*)sExtraSlots, 0, l.slots * SHADOW_STUB_SIZE);
        LOG_DEBUG("Shadow: overlay slots at 0x%x, 0x%x bytes\n", sExtraSlots, l.slots * SHADOW_STUB_SIZE);
    }

    memset(space, 0, size);
    memcpy(space, SECTION_START(patch_shadow), SECTION_SIZE(patch_shadow));
    u32* words = (u32*)space;
    auto run = [&](const void* f) { return runAddress + ((u32)f - (u32)SECTION_START(patch_shadow)); };
    auto at = [&](const void* f) { return (u32*)((u8*)space + ((u32)f - (u32)SECTION_START(patch_shadow))); };
    u32 record = run((const void*)shadow_record);
    u32 recordThumb = run((const void*)shadow_record_thumb);
    u32 copied[4] = { run((const void*)shadow_copied), run((const void*)shadow_filled8),
        run((const void*)shadow_filled16), run((const void*)shadow_filled32) };

    u32 slotsOffset = codeSize + l.fixed * SHADOW_STUB_SIZE;
    u32 groupsOffset = slotsOffset + slotBytes;
    u32 recordsOffset = groupsOffset + l.groups * SHADOW_GROUP_SIZE;
    u32* groups = (u32*)((u8*)space + groupsOffset);
    u8* records = (u8*)space + recordsOffset;
    static u32 copyValue[SHADOW_MAX_SITES]; // an overlay copy call's halfwords to put, 0 if none

    u32 made = 0, fixedMade = 0, failed = 0, overlaySites = 0;
    for (u32 i = 0; i < count; i++)
    {
        const Entry& e = sEntries[i];
        copyValue[i] = 0;
        // an overlay's sites are made below, as records
        if (e.overlay && (leftOut(e.overlay) || e.type != ENTRY_COPY))
            continue;
        const bool fixed = true;
        u32 stubOffset = codeSize + fixedMade * SHADOW_STUB_SIZE;
        u32 stubRun = runAddress + stubOffset;
        u32* stub = (u32*)((u8*)space + stubOffset);
        u32 value = 0;
        bool ok = false;
        if (e.type == ENTRY_ARM)
        {
            u32 op = e.orig;
            u32 toStub, toRecord;
            if (armBranch(e.final, stubRun, toStub) && armBranch(stubRun + 8, record, toRecord))
            {
                if (fixed)
                {
                    stub[0] = op;
                    stub[1] = 0xE52DE008; // str lr, [sp, #-8]!
                    stub[2] = 0xEB000000 | toRecord; // bl shadow_record
                    stub[3] = e.final + 4;
                }
                value = (op & 0xF0000000) | 0x0A000000 | toStub;
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
                if (fixed)
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
                }
                value = hi | (lo << 16);
                ok = true;
            }
        }
        else if (e.type == ENTRY_THUMB_PREV)
        {
            // push {lr} / prev / store / blx record / pop {pc}
            u16 hi, lo, rhi, rlo;
            if (thumbCall(e.final, stubRun, false, hi, lo) && thumbCall(stubRun + 6, recordThumb, true, rhi, rlo))
            {
                if (fixed)
                {
                    u32 prev = e.orig & 0xFFFF, store = e.orig >> 16;
                    stub[3] = 0;
                    if (thumbLiteralLoad(prev))
                    {
                        stub[3] = thumbLiteralOf(e.initial);
                        prev = 0x4802 | (prev & 0x0700);
                    }
                    stub[0] = 0xB500 | (prev << 16);
                    stub[1] = store | (rhi << 16);
                    stub[2] = rlo | (0xBD00 << 16);
                }
                value = hi | (lo << 16);
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
                        value = hi | (lo << 16);
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
                        value = cond | (call ? 0x0B000000 : 0x0A000000) | toStub;
                        ok = true;
                    }
                }
            }
        }
        if (!ok)
        {
            failed++;
            if (fixed)
                memset(stub, 0, SHADOW_STUB_SIZE);
            continue;
        }
        made++;
        if (!e.overlay)
        {
            // the static arm9: in place now
            u16* h = (u16*)e.initial;
            h[0] = value;
            h[1] = value >> 16;
            fixedMade++;
            continue;
        }
        // an overlay's copy call: its stub now, its record with its group's
        copyValue[i] = value;
        fixedMade++;
        overlaySites++;
    }

    // the overlays' groups, as layoutFor makes them, and their records
    u32 group = 0, recordByte = 0;
    for (u32 i = 0; i < count;)
    {
        u32 overlay = sEntries[i].overlay;
        if (!overlay || leftOut(overlay))
        {
            i++;
            continue;
        }
        u32 key = groupKey(sEntries[i]);
        u32 end = i;
        while (end < count && (groupKey(sEntries[end]) == key || !sEntries[end].overlay || leftOut(sEntries[end].overlay)))
            end++;
        u32 base = 0, spanSize = 0;
        if (spanOf(overlay, base, spanSize))
            base += windowOf(sEntries[i]) << SHADOW_WINDOW_SHIFT;
        else
        {
            base = 0xFFFFFFFF;
            for (u32 j = i; j < end; j++)
                if (sEntries[j].overlay == overlay && sEntries[j].final < base)
                    base = sEntries[j].final & ~3u;
        }
        u32* g = groups + group * (SHADOW_GROUP_SIZE / 4);
        u32 slot0 = (elsewhere ? sExtraSlots : runAddress + slotsOffset) + l.slotStart[group] * SHADOW_STUB_SIZE;
        g[0] = overlay - 1;
        g[1] = base;
        g[2] = runAddress + recordsOffset + recordByte;
        g[3] = slot0;
        u16* rec = (u16*)((u8*)records + recordByte);
        u32 hash = 2166136261u, n = 0;
        u32 counts[4];
        static const u32 order[4] = { ENTRY_ARM, ENTRY_THUMB, ENTRY_THUMB2, ENTRY_THUMB_PREV };
        for (u32 t = 0; t < 4; t++)
        {
            counts[t] = 0;
            for (u32 j = i; j < end; j++)
            {
                const Entry& e = sEntries[j];
                if (e.overlay != overlay || e.type != order[t])
                    continue;
                u32 scale = t == 0 ? 4 : 2;
                u32 off = e.final - base;
                u32 stubRun = slot0 + n * SHADOW_STUB_SIZE;
                u32 x;
                u16 hi, lo;
                bool ok = e.final >= base && off % scale == 0 && off / scale <= 0xFFFF;
                if (ok && t == 0)
                    ok = armBranch(e.final, stubRun, x) && armBranch(stubRun + 8, record, x);
                else if (ok)
                    ok = thumbCall(e.final, stubRun, false, hi, lo) && thumbCall(stubRun + 4, recordThumb, true, hi, lo)
                        && thumbCall(stubRun + 10, recordThumb, true, hi, lo);
                if (!ok)
                {
                    failed++;
                    continue;
                }
                rec[n++] = off / scale;
                hash = (hash ^ e.orig) * 16777619u;
                counts[t]++;
                made++;
                overlaySites++;
            }
        }
        g[4] = hash;
        g[5] = counts[0] | (counts[1] << 16);
        g[6] = counts[2] | (counts[3] << 16);
        recordByte += (n * 2 + 3) & ~3u;
        u32* copies = (u32*)((u8*)records + recordByte);
        g[7] = runAddress + recordsOffset + recordByte;
        copies[0] = 0;
        for (u32 j = i; j < end; j++)
        {
            const Entry& e = sEntries[j];
            if (e.overlay != overlay || e.type != ENTRY_COPY || !copyValue[j])
                continue;
            u32* c = copies + 1 + copies[0] * 3;
            c[0] = e.final;
            c[1] = e.orig;
            c[2] = copyValue[j];
            copies[0]++;
        }
        recordByte += 4 + copies[0] * 12;
        group++;
        i = end;
    }
    u32 groupsMade = group;
    words[SHADOW_COUNT_FIELD / 4] = made;
    words[SHADOW_STUBS_OFFSET_FIELD / 4] = codeSize;
    *at(&shadow_groups) = runAddress + groupsOffset;
    *at(&shadow_group_count) = groupsMade;
    sVblankRoutine = 0;
    if (groupsMade && sOverlayHookTarget)
        *sOverlayHookTarget = run((const void*)shadow_overlay_loaded);
    else if (groupsMade)
        sVblankRoutine = run((const void*)shadow_vblank);

    // region 2 off: the space is in the arm7's corner the sdk guards from the arm9.
    // Only where region 2 is that guard: the twl sdk's (ds mode too) is the 4 KB
    // shared page, and its main memory region takes the corner in already.
    bool guards = false;
    for (u32 i = 0; i < sRegion2LiteralCount; i++)
    {
        u32 v = *(const u32*)sRegion2Literal[i];
        u32 base = v & ~0xFFFu, bytes = 2u << ((v >> 1) & 0x1F);
        u32 off = runAddress & 0x3FFFFF;
        for (u32 mirror = 0x02000000; mirror < 0x03000000; mirror += 0x400000)
            if ((v & 1) && mirror + off < base + bytes && base < mirror + off + size)
                guards = true;
    }
    for (u32 i = 0; guards && i < sRegion2LiteralCount; i++)
        *(u32*)sRegion2Literal[i] = 0;
    for (u32 i = 0; guards && i < sRegion2McrCount; i++)
        *(u32*)sRegion2Mcr[i] = 0xE1A00000; // mov r0, r0
    LOG_DEBUG("Shadow: region 2 %s\n", guards ? "off (it guarded the space)" : "left (not over the space)");
    LOG_DEBUG("Shadow at 0x%x: %u stubs of %u (%u out of reach)\n", runAddress, made, sEntryCount, failed);
    LOG_DEBUG("Shadow: %u overlays, %u sites in %u slots, %u static from overlays (%u scanned, %u lz40, %u not unpacked, uncompress 0x%x)\n",
        groupsMade, overlaySites, l.slots, sStaticFromOverlays, sOverlaysScanned, sOverlaysLz40, sOverlaysPacked, (u32)sUncompress);
    return made != 0;
}
