// The restore test. Two halves:
//  - after "Test: load + compare" loads slot 1, the memory and then (as the menu
//    leaves) the registers are checked against slot 1: what the load put back
//    is what landed and stayed;
//  - the run that follows freezes itself after TEST_FRAMES frames and writes
//    slot 3, which is compared with slot 2, written the same way after "Test:
//    save + reference": a complete restore runs the same as the original did.
// Both report to /_pico/rtslog.txt: the load check in its first 8 KB, the run
// comparison in the second.

#include <string.h>
#include "rtsmenu.h"
#include "state.h"
#include "stateint.h"
#include "census.h"

int gCheckOnLeave;

// ---------------------------------------------------------------- the log

#define LOG_SIZE 0x1000
static char sLog[LOG_SIZE];
static u32 sLogLen;

static void logStr(const char* s)
{
    while (*s && sLogLen < LOG_SIZE - 2)
        sLog[sLogLen++] = *s++;
}

static void logHex(u32 v, int digits)
{
    char b[9];
    for (int i = digits - 1; i >= 0; i--, v >>= 4)
        b[i] = "0123456789ABCDEF"[v & 15];
    b[digits] = 0;
    logStr(b);
}

static void logDec(u32 v)
{
    char b[11];
    int n = 10;
    b[n] = 0;
    do
        b[--n] = '0' + v % 10;
    while (v /= 10);
    logStr(b + n);
}

static void logEnd(void)
{
    logStr("\r\n");
}

static void logName(const char* name)
{
    u32 start = sLogLen;
    logStr(name);
    while (sLogLen < start + 14)
        logStr(" ");
}

static void logGame(void* block)
{
    const u8* cart = (const u8*)block + HEADER_FROM_BLOCK;
    char title[17];
    memcpy(title, cart, 12);
    title[12] = 0;
    logStr("game: ");
    logStr(title);
    logStr(" (");
    char code[5] = { cart[12], cart[13], cart[14], cart[15], 0 };
    logStr(code);
    logStr(")");
    logEnd();
}

#define PREVIOUS "(previous) "

// The log file: five 4 KB sections, padded with spaces. 0 the latest load
// check, 1 the latest run comparison, 2 and 3 the ones before them, 4 the
// register-write census. Writing section 0 or 1 moves what it held to 2 or 3
// first. Uses __scratch.
static int logWrite(int section)
{
    if (!sRts->logSector || sRts->logSectorCount * 512 < (u32)(section + 1) * LOG_SIZE)
        return 0;
    u8* old = (u8*)__scratch;
    u8* moved = (u8*)__scratch + LOG_SIZE;
    u32 sector = sRts->logSector + section * (LOG_SIZE / 512);
    if (section < 2 && sRts->logSectorCount * 512 >= 4 * LOG_SIZE &&
        command(RTS_MENU_STATE_SDREAD, sector, (u32)old, LOG_SIZE / 512) == RTS_MENU_STATE_DONE &&
        old[0] == 'R' && old[1] == 'T' && old[2] == 'S')
    {
        u32 n = sizeof(PREVIOUS) - 1;
        memcpy(moved, PREVIOUS, n);
        memcpy(moved + n, old, LOG_SIZE - n);
        moved[LOG_SIZE - 2] = '\r';
        moved[LOG_SIZE - 1] = '\n';
        command(RTS_MENU_STATE_SDWRITE, sector + 2 * (LOG_SIZE / 512), (u32)moved, LOG_SIZE / 512);
    }
    for (u32 i = sLogLen; i < LOG_SIZE; i++)
        sLog[i] = ' ';
    sLog[LOG_SIZE - 2] = '\r';
    sLog[LOG_SIZE - 1] = '\n';
    int ok = command(RTS_MENU_STATE_SDWRITE, sector, (u32)sLog, LOG_SIZE / 512) == RTS_MENU_STATE_DONE;
    sLogLen = 0;
    return ok;
}

// ---------------------------------------------------------------- comparing

#define CHUNK 0x2000
#define BUF_A ((u8*)__scratch)          // the file side
#define BUF_B ((u8*)__scratch + CHUNK)  // the other side

enum { SRC_ARM9, SRC_ARM7, SRC_VRAM, SRC_SLOT3, SRC_STASH };

typedef struct
{
    const char* name;
    u32 offset;     // in the state file
    u32 size;
    u32 address;    // where offset 0 is in memory: for the skips and the report
    u8 source;      // where the other side comes from
    u8 bank;        // SRC_VRAM: index in sBanks
} part_t;

typedef struct
{
    u32 start, end; // addresses left out, [start, end)
} skip_t;

#define MAX_PARTS 24
#define MAX_SKIPS 6
static part_t sParts[MAX_PARTS];
static int sPartCount;
static skip_t sSkips[MAX_SKIPS];
static int sSkipCount;

static void addPart(const char* name, u32 offset, u32 size, u32 address, int source, int bank)
{
    if (sPartCount < MAX_PARTS)
        sParts[sPartCount++] = (part_t){ name, offset, size, address, (u8)source, (u8)bank };
}

static void addSkip(u32 start, u32 end)
{
    if (start < end && sSkipCount < MAX_SKIPS)
        sSkips[sSkipCount++] = (skip_t){ start, end };
}

static int skipped(u32 address)
{
    for (int k = 0; k < sSkipCount; k++)
        if (address >= sSkips[k].start && address < sSkips[k].end)
            return 1;
    return 0;
}

static u32 sDone, sTotal;
static void (*sProgress)(u32 done, u32 total);

static u32 totalChunks(void)
{
    u32 n = 0;
    for (int i = 0; i < sPartCount; i++)
        n += (sParts[i].size + CHUNK - 1) / CHUNK;
    return n;
}

// One part of slot slotA against its source. Logs a line; 1 if it differs.
static int comparePart(int slotA, const part_t* p)
{
    u32 diffBytes = 0, firstDiff = 0;
    u32 blocks[6];
    int blockCount = 0;
    int failed = 0;
    for (u32 off = 0; off < p->size; off += CHUNK)
    {
        u32 n = p->size - off < CHUNK ? p->size - off : CHUNK;
        if (!readSlot(slotA, p->offset + off, (u32)BUF_A, n))
        {
            failed = 1;
            break;
        }
        const volatile u8* b = BUF_B;
        switch (p->source)
        {
            case SRC_ARM9:
                b = (const volatile u8*)(p->address + off);
                break;
            case SRC_ARM7:
                if (command(RTS_MENU_STATE_ARM7_COPY, p->address + off, (u32)BUF_B, n) != RTS_MENU_STATE_DONE)
                    failed = 1;
                break;
            case SRC_VRAM:
            {
                vu8* cnt = (vu8*)(0x04000240 + sBanks[p->bank].cnt);
                u8 old = *cnt;
                *cnt = 0x80; // enabled, lcdc
                copyWords((u32*)BUF_B, (const volatile u32*)(sBanks[p->bank].lcdc + off), n);
                *cnt = old;
                break;
            }
            case SRC_SLOT3:
                if (!readSlot(3, p->offset + off, (u32)BUF_B, n))
                    failed = 1;
                break;
            case SRC_STASH:
                if (command(RTS_MENU_STATE_SDREAD, sRts->stashSector + (p->address - RTS_BASE + off) / 512,
                        (u32)BUF_B, n / 512) != RTS_MENU_STATE_DONE)
                    failed = 1;
                break;
        }
        if (failed)
            break;
        for (u32 i = 0; i < n; i++)
        {
            if (BUF_A[i] == b[i])
                continue;
            u32 address = p->address + off + i;
            if (skipped(address))
                continue;
            if (!diffBytes)
                firstDiff = address;
            diffBytes++;
            u32 block = address & ~511;
            if (blockCount < 6 && (blockCount == 0 || blocks[blockCount - 1] != block))
                blocks[blockCount++] = block;
        }
        if (sProgress)
            sProgress(++sDone < sTotal ? sDone : sTotal, sTotal);
    }

    logName(p->name);
    if (failed)
    {
        logStr("could not read");
    }
    else if (!diffBytes)
    {
        logStr("same");
    }
    else
    {
        logDec(diffBytes);
        logStr(" bytes differ, first at ");
        logHex(firstDiff, 8);
        logStr("; in the 512-byte blocks at");
        for (int i = 0; i < blockCount; i++)
        {
            logStr(" ");
            logHex(blocks[i], 8);
        }
        if (blockCount == 6)
            logStr(" ...");
    }
    logEnd();
    return failed || diffBytes != 0;
}

// Words of a slot 2 and slot 3 section side by side (registers, header).
// Bits that only measure how long the game sat paused before the snapshot,
// not what was restored: left out of the comparison.
static u32 timeOnlyBits(u32 section, u32 o)
{
    if (section == STATE_ARM9_IO)
    {
        if (o == 0x004) return 0xFFFF0007;              // VCOUNT, the display status flags
        if (o >= 0x100 && o < 0x110) return 0x0000FFFF; // timer counts
        if (o == 0x130 || o == 0x134) return 0xFFFFFFFF; // keys
        if (o == 0x214) return 0xFFFFFFFF;              // IF
        if (o >= 0x600 && o < 0x6A4) return 0xFFFFFFFF; // 3d status, ram counts, results
    }
    if (section == STATE_ARM7_IO)
    {
        if (o == ARM7_CTX_IF) return 0xFFFFFFFF;
        if (o >= ARM7_CTX_TIMERS && o < ARM7_CTX_TIMERS + 16) return 0x0000FFFF;
        if (o == ARM7_CTX_DISPSTAT_IPCSYNC) return 0x00000007; // the arm7's display status flags
    }
    return 0;
}

static int compareWords(const char* name, u32 offset, u32 size, u32 from)
{
    if (!readSlot(2, offset, (u32)BUF_A, size) || !readSlot(3, offset, (u32)BUF_B, size))
    {
        logName(name);
        logStr("could not read");
        logEnd();
        return 1;
    }
    u32 count = 0;
    for (u32 i = from; i < size; i += 4)
    {
        u32 a = *(u32*)(BUF_A + i), b = *(u32*)(BUF_B + i);
        if (!((a ^ b) & ~timeOnlyBits(offset, i)))
            continue;
        if (count++ < 24)
        {
            logName(count == 1 ? name : "");
            logStr("+");
            logHex(i, 4);
            logStr(": ");
            logHex(a, 8);
            logStr(" -> ");
            logHex(b, 8);
            logEnd();
        }
    }
    if (!count)
    {
        logName(name);
        logStr("same");
        logEnd();
    }
    else if (count > 24)
    {
        logName("");
        logStr("... ");
        logDec(count);
        logStr(" words in all");
        logEnd();
    }
    return count != 0;
}

static void openPermissions(u32* saved)
{
    *saved = cp15Read(6);
    cp15WritePermissions(0x11111111);
}

static void closePermissions(u32 saved)
{
    cp15WritePermissions(saved);
}

// The arm7's irq sp at its command loop, from a slot's arm7 context.
static u32 arm7Sp(int slot)
{
    if (!readSlot(slot, STATE_ARM7_IO, (u32)BUF_A, 512))
        return 0;
    return *(u32*)(BUF_A + ARM7_CTX_SP_IRQ);
}

// The arm7 wram the stub itself uses while the menu runs: its patch space,
// when it is there, and its own frames below the command loop's sp.
static void addArm7WramSkips(u32 sp)
{
    if (sRts->arm7PatchStart >= 0x03800000 && sRts->arm7PatchStart < 0x03810000)
        addSkip(sRts->arm7PatchStart, sRts->arm7PatchStart + sRts->arm7PatchSize);
    if (sp)
        addSkip(sp - 0x200, sp);
}

// ---------------------------------------------------------------- load check

// dsi mode: the new wram's slots that are mapped, through their cpu (or slot 3)
static void addTwlParts(int src)
{
    static const char* const names[3] = { "wram A", "wram B", "wram C" };
    for (int s = 0; TWL_MODE && s < TWL_SLOTS; s++)
    {
        int arm7 = 0;
        u32 offset, size, at = twlSlot(s, &arm7, &offset, &size);
        int from = src == SRC_SLOT3 ? src : arm7 ? SRC_ARM7 : SRC_ARM9;
        part_t* last = &sParts[sPartCount - 1];
        // a bank's slots in order are one part (MAX_PARTS)
        if (at && last->source == from && last->offset + last->size == offset && last->address + last->size == at
            && last->name == names[s < 4 ? 0 : s < 12 ? 1 : 2])
            last->size += size;
        else if (at)
            addPart(names[s < 4 ? 0 : s < 12 ? 1 : 2], offset, size, at, from, 0);
    }
}

u32 checkLoadedMemory(void* block, void (*progress)(u32 done, u32 total))
{
    sPartCount = 0;
    sLogLen = 0;
    logStr("RTS load check: slot 1 against memory after the load");
    logEnd();
    logGame(block);
    logEnd();

    u32 dtcm = cp15Read(1) & ~0xFFF;
    u32 mode = gGameScreen.vramcnt[7] & 3;
    addPart("itcm", STATE_ITCM, 0x8000, 0x01FF8000, SRC_ARM9, 0);
    addPart("dtcm", STATE_DTCM, 0x4000, dtcm, SRC_ARM9, 0);
    addPart("palettes", STATE_PALETTE, 0x800, 0x05000000, SRC_ARM9, 0);
    addPart("oam", STATE_OAM, 0x800, 0x07000000, SRC_ARM9, 0);
    for (u32 half = 0; half < 2; half++)
    {
        int arm7 = mode == 3 || (mode == 1 && half == 0) || (mode == 2 && half == 1);
        u32 address = 0x037F8000 + (mode == 0 || mode == 3 ? half * 0x4000 : 0);
        addPart(half ? "shared wram 2" : "shared wram 1", STATE_SHARED_WRAM + half * 0x4000, 0x4000,
            address, arm7 ? SRC_ARM7 : SRC_ARM9, 0);
    }
    addPart("arm7 wram", STATE_ARM7_WRAM, 0x10000, 0x03800000, SRC_ARM7, 0);
    static const char* const bankNames[9] =
        { "vram A (copy)", "vram B", "vram C", "vram D", "vram E", "vram F", "vram G", "vram H", "vram I" };
    u32 offset = STATE_VRAM;
    for (int i = 0; i < 9; i++)
    {
        if (i == 0)
            addPart(bankNames[0], offset, sBanks[0].size, (u32)__vram_copy, SRC_ARM9, 0);
        else
            addPart(bankNames[i], offset, sBanks[i].size, sBanks[i].lcdc, SRC_VRAM, i);
        offset += sBanks[i].size;
    }
    addPart("main ram", STATE_MAIN_RAM, gMainSize, 0x02000000, SRC_ARM9, 0);
    u32 dtcmEnd, dtcmMain = dtcmOverMain(&dtcmEnd);
    if (dtcmMain) // the ram under the dtcm, as the arm7 sees it
        addPart("main (dtcm)", STATE_MAIN_RAM + (dtcmMain - 0x02000000), dtcmEnd - dtcmMain, dtcmMain, SRC_ARM7, 0);
    addTwlParts(SRC_ARM9);
    addPart("main (stash)", STATE_MAIN_RAM + (RTS_BASE - 0x02000000), RTS_STASH_SIZE, RTS_BASE, SRC_STASH, 0);

    u32 keep[KEEP_RANGES][2];
    mainRamKeep(block, keep);
    u32 sp = arm7Sp(1);

    sProgress = progress;
    sDone = 0;
    sTotal = totalChunks();
    u32 permissions;
    openPermissions(&permissions);
    u32 differ = 0;
    for (int i = 0; i < sPartCount; i++)
    {
        sSkipCount = 0;
        const part_t* p = &sParts[i];
        if (p->address == 0x05000000)
            addSkip(0x05000400, 0x05000402); // the sub backdrop: the menu's until it leaves
        if (p->address == 0x03800000)
            addArm7WramSkips(sp);
        if (p->address == 0x02000000)
        {
            for (int k = 0; k < KEEP_RANGES; k++)
                addSkip(0x02000000 + keep[k][0], 0x02000000 + keep[k][1]);
            addSkip(dtcmMain, dtcmEnd); // its own part
        }
        differ += comparePart(1, p);
    }
    closePermissions(permissions);
    logStr("Left out: the menu's backdrop, the loader's block, arm7 patch space and census, the arm7 stub's frames.");
    logEnd();
    logStr("memory parts that differ: ");
    logDec(differ);
    logEnd();
    logEnd();
    return differ;
}

// ---------------------------------------------------------------- registers

typedef struct
{
    u16 offset;     // from 0x04000000
    u8 size;        // 1, 2 or 4
    u32 mask;       // the bits a load sets
    const char* name;
} reg_t;

static const reg_t sRegs[] =
{
    { 0x000, 4, 0xFFFFFFFF, "DISPCNT" },
    { 0x004, 2, 0xFFB8, "DISPSTAT" },
    { 0x008, 4, 0xFFFFFFFF, "BG0CNT/BG1CNT" },
    { 0x00C, 4, 0xFFFFFFFF, "BG2CNT/BG3CNT" },
    { 0x048, 4, 0x3F3F3F3F, "WININ/WINOUT" },
    { 0x050, 2, 0x3FFF, "BLDCNT" },
    { 0x060, 2, 0x4FFF, "DISP3DCNT" },
    { 0x064, 4, 0x7FFFFFFF, "DISPCAPCNT" },
    { 0x06C, 2, 0xC01F, "MASTER_BRIGHT" },
    { 0x0E0, 4, 0xFFFFFFFF, "DMA0FILL" },
    { 0x0E4, 4, 0xFFFFFFFF, "DMA1FILL" },
    { 0x0E8, 4, 0xFFFFFFFF, "DMA2FILL" },
    { 0x0EC, 4, 0xFFFFFFFF, "DMA3FILL" },
    { 0x102, 2, 0x00C7, "TM0CNT_H" },
    { 0x106, 2, 0x00C7, "TM1CNT_H" },
    { 0x10A, 2, 0x00C7, "TM2CNT_H" },
    { 0x10E, 2, 0x00C7, "TM3CNT_H" },
    { 0x180, 2, 0x4F00, "IPCSYNC" },
    { 0x184, 2, 0x8404, "IPCFIFOCNT" },
    { 0x204, 2, 0xFFFF, "EXMEMCNT (not set by a load)" },
    { 0x208, 4, 0x0001, "IME (not set by a load)" },
    { 0x210, 4, 0xFFFFFFFF, "IE" },
    { 0x240, 4, 0xFFFFFFFF, "VRAMCNT A-D" },
    { 0x244, 4, 0xFFFFFFFF, "VRAMCNT E-G, WRAMCNT" },
    { 0x248, 2, 0xFFFF, "VRAMCNT H-I" },
    { 0x280, 2, 0x0003, "DIVCNT" },
    { 0x290, 4, 0xFFFFFFFF, "DIV_NUMER low" },
    { 0x294, 4, 0xFFFFFFFF, "DIV_NUMER high" },
    { 0x298, 4, 0xFFFFFFFF, "DIV_DENOM low" },
    { 0x29C, 4, 0xFFFFFFFF, "DIV_DENOM high" },
    { 0x2A0, 4, 0xFFFFFFFF, "DIV_RESULT low" },
    { 0x2A4, 4, 0xFFFFFFFF, "DIV_RESULT high" },
    { 0x2B0, 2, 0x0001, "SQRTCNT" },
    { 0x2B4, 4, 0xFFFFFFFF, "SQRT_RESULT" },
    { 0x2B8, 4, 0xFFFFFFFF, "SQRT_PARAM low" },
    { 0x2BC, 4, 0xFFFFFFFF, "SQRT_PARAM high" },
    { 0x304, 2, 0xFFFF, "POWCNT1" },
    { 0x1000, 4, 0xFFFFFFFF, "DISPCNT B" },
    { 0x1008, 4, 0xFFFFFFFF, "B BG0CNT/BG1CNT" },
    { 0x100C, 4, 0xFFFFFFFF, "B BG2CNT/BG3CNT" },
    { 0x1048, 4, 0x3F3F3F3F, "B WININ/WINOUT" },
    { 0x1050, 2, 0x3FFF, "B BLDCNT" },
    { 0x106C, 2, 0xC01F, "B MASTER_BRIGHT" },
};

static u32 sRegDiffs;

static void regMismatch(const char* name, u32 saved, u32 now)
{
    sRegDiffs++;
    logName(name);
    logStr(" saved ");
    logHex(saved, 8);
    logStr(" now ");
    logHex(now, 8);
    logEnd();
}

// The arm7 context words a load sets, with the bits to compare.
static u32 arm7CtxMask(u32 offset)
{
    if (offset < 0x70)
        return 0xFFFFFFFF;          // banked registers, IE, the stub's saved IME/sound/IF, POWCNT2, SOUNDBIAS
    if (offset < 0x80)
        return 0x00C70000;          // TMxCNT: the control half
    if (offset < 0xC0)
        return 0x7FFFFFFF;          // SOUNDxCNT, stopped by the load
    if (offset < 0xE8)
        return 0xFFFFFFFF;          // the words under the wait loop, the stub address
    if (offset == 0xE8)
        return 0x4F00FFB8;          // DISPSTAT, IPCSYNC
    if (offset == 0xEC)
        return 0xCF038404;          // IPCFIFOCNT, SPICNT
    if (offset == 0xF0)
        return 0x8F8F;              // SNDCAPxCNT
    return 0;                       // the dma halves: below
}

u32 checkLoadedRegisters(void)
{
    sRegDiffs = 0;
    u8* io = BUF_A;                     // slot 1's arm9 io dump, 0x1200
    state_header_t* h = (state_header_t*)(BUF_A + 0x1200);
    u8* ctx = BUF_A + 0x1400;           // slot 1's arm7 context
    u8* now7 = BUF_A + 0x1600;          // the arm7's now
    if (!readSlot(1, STATE_ARM9_IO, (u32)io, 0x1200) ||
        !readSlot(1, STATE_HEADER, (u32)h, 512) ||
        !readSlot(1, STATE_ARM7_IO, (u32)ctx, 512))
    {
        logStr("registers: could not read slot 1");
        logEnd();
        logWrite(0);
        return 1;
    }

    logStr("Registers after the menu left (saved = slot 1, now = the hardware):");
    logEnd();
    for (u32 i = 0; i < sizeof(sRegs) / sizeof(sRegs[0]); i++)
    {
        const reg_t* r = &sRegs[i];
        u32 saved, now;
        if (r->size == 4)
        {
            saved = *(u32*)(io + r->offset);
            now = *(vu32*)(0x04000000 + r->offset);
        }
        else
        {
            saved = *(u16*)(io + r->offset);
            now = *(vu16*)(0x04000000 + r->offset);
        }
        if ((saved ^ now) & r->mask)
            regMismatch(r->name, saved, now);
    }
    // dma: the repeating display ones set again in full, the rest stopped
    for (int ch = 0; ch < 4; ch++)
    {
        u32 base = 0xB0 + ch * 12;
        u32 control = *(u32*)(io + base + 8);
        static const char* const names[4] = { "DMA0", "DMA1", "DMA2", "DMA3" };
        vu32* live = (vu32*)(0x04000000 + base);
        if (dmaRestored(control))
        {
            for (int w = 0; w < 3; w++)
                if (live[w] != *(u32*)(io + base + w * 4))
                    regMismatch(names[ch], *(u32*)(io + base + w * 4), live[w]);
        }
        else
        {
            // stopped, with the rest as the game had it
            for (int w = 0; w < 3; w++)
            {
                u32 saved = *(u32*)(io + base + w * 4);
                if (w == 2)
                    saved &= ~(1u << 31);
                if (live[w] != saved)
                    regMismatch(names[ch], saved, live[w]);
            }
        }
    }
    // cp15: not set by a load, the same setup or the load would have refused
    static const char* const cp15Names[] = { "cp15 control", "cp15 dtcm", "cp15 itcm",
        "cp15 dcache", "cp15 icache", "cp15 wbuffer", "cp15 data perm", "cp15 insn perm" };
    const u32* expect = &h->control;
    for (int i = 0; i < 16; i++)
        if (cp15Read(i) != expect[i])
            regMismatch(i < 8 ? cp15Names[i] : "cp15 region", expect[i], cp15Read(i));

    // the arm7, through its own context as it is now
    if (*(u32*)(ctx + ARM7_CTX_MAGIC) == RTS_MENU_STATE_ARM7_SAVE)
    {
        memset(now7, 0, 0x100);
        command(RTS_MENU_STATE_ARM7_SAVE, 0, (u32)now7, 0);
        int ext = *(u32*)(ctx + ARM7_CTX_EXT_MAGIC) == *(u32*)(now7 + ARM7_CTX_EXT_MAGIC);
        for (u32 o = 4; o < 0xF4; o += 4)
        {
            u32 mask = arm7CtxMask(o);
            if (o >= 0xE8 && !ext)
                mask = 0;
            u32 saved = *(u32*)(ctx + o), now = *(u32*)(now7 + o);
            if ((saved ^ now) & mask)
            {
                char name[16] = "arm7 ctx +";
                static const char hex[] = "0123456789ABCDEF";
                name[10] = hex[(o >> 4) & 15];
                name[11] = hex[o & 15];
                name[12] = 0;
                regMismatch(name, saved, now);
            }
        }
        // dma channels the state had off are stopped now
        for (u32 o = 0xF4; ext && o < 0xFC; o += 4)
        {
            u32 saved = *(u32*)(ctx + o), now = *(u32*)(now7 + o);
            if (now & ~saved & 0x80008000)
                regMismatch(o == 0xF4 ? "arm7 DMA0/1CNT_H" : "arm7 DMA2/3CNT_H", saved, now);
        }
    }
    logStr("registers that differ: ");
    logDec(sRegDiffs);
    logEnd();
    logWrite(0);
    return sRegDiffs;
}

// ---------------------------------------------------------------- run comparison

u32 compareTestSlots(void* block, void (*progress)(u32 done, u32 total))
{
    sPartCount = 0;
    sLogLen = 0;
    logStr("RTS run comparison: slot 2 (after save + reference) against slot 3 (after load + compare),");
    logEnd();
    logStr("each written ");
    logDec(TEST_FRAMES);
    logStr(" frames after the game resumed from slot 1. 'a -> b' is slot 2 -> slot 3.");
    logEnd();
    logGame(block);
    logEnd();

    u32 differ = 0;
    differ += compareWords("header", STATE_HEADER, 512, 0x1C); // from the cpu registers on
    differ += compareWords("arm9 io", STATE_ARM9_IO, 0x1200, 0);
    differ += compareWords("arm7 context", STATE_ARM7_IO, 512, 4);

    addPart("itcm", STATE_ITCM, 0x8000, 0x01FF8000, SRC_SLOT3, 0);
    addPart("dtcm", STATE_DTCM, 0x4000, cp15Read(1) & ~0xFFF, SRC_SLOT3, 0);
    addPart("palettes", STATE_PALETTE, 0x800, 0x05000000, SRC_SLOT3, 0);
    addPart("oam", STATE_OAM, 0x800, 0x07000000, SRC_SLOT3, 0);
    addPart("shared wram", STATE_SHARED_WRAM, 0x8000, 0x037F8000, SRC_SLOT3, 0);
    addPart("arm7 wram", STATE_ARM7_WRAM, 0x10000, 0x03800000, SRC_SLOT3, 0);
    static const char* const bankNames[9] =
        { "vram A", "vram B", "vram C", "vram D", "vram E", "vram F", "vram G", "vram H", "vram I" };
    u32 offset = STATE_VRAM;
    for (int i = 0; i < 9; i++)
    {
        addPart(bankNames[i], offset, sBanks[i].size, sBanks[i].lcdc, SRC_SLOT3, 0);
        offset += sBanks[i].size;
    }
    addPart("main ram", STATE_MAIN_RAM, gMainSize, 0x02000000, SRC_SLOT3, 0);
    addTwlParts(SRC_SLOT3);

    u32 keep[KEEP_RANGES][2];
    mainRamKeep(block, keep);
    u32 sp = arm7Sp(2);
    sProgress = progress;
    sDone = 0;
    sTotal = totalChunks();
    for (int i = 0; i < sPartCount; i++)
    {
        sSkipCount = 0;
        const part_t* p = &sParts[i];
        if (p->address == 0x03800000)
            addArm7WramSkips(sp);
        if (p->address == 0x02000000)
            for (int k = 1; k < KEEP_RANGES; k++) // the stash window is game memory in the files
                addSkip(0x02000000 + keep[k][0], 0x02000000 + keep[k][1]);
        differ += comparePart(2, p);
    }
    logStr("Left out: the loader's block, arm7 patch space and census, the arm7 stub's frames, and");
    logEnd();
    logStr("what only measures the pause (VCOUNT, display status flags, timer counts, keys,");
    logEnd();
    logStr("IF, 3D status).");
    logEnd();
    logStr("parts that differ: ");
    logDec(differ);
    logEnd();
    logWrite(1);
    return differ;
}

// The census report into the log's fifth section; the counts restart. Returns
// 0 when the log could not be written.
int censusWriteLog(void* block, char* line1, char* line2)
{
    sLogLen = 0;
    logStr("RTS register-write census");
    logEnd();
    logGame(block);
    logEnd();
    sLogLen += censusReport(sLog + sLogLen, LOG_SIZE - sLogLen, line1, line2);
    int ok = logWrite(4);
    censusRestart();
    return ok;
}
