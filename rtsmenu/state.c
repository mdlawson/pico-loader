// Writing a save state (state.h) while the game is frozen. The arm7 does the sd
// card side through the menu command channel; what only the arm9 can read
// (tcms, vram banks, palettes, oam, registers) goes through __scratch first.

#include <string.h>
#include "rtsmenu.h"
#include "state.h"

static u32 sIoDump[STATE_ARM9_IO_SIZE / 4];

// The readable arm9 registers, as offsets from 0x04000000 (end exclusive). Left
// out: the display fifo at 0x068 and the geometry command ports.
static const u16 sIoRanges[][2] =
{
    { 0x000, 0x068 }, { 0x06C, 0x070 },   // display A, capture, master bright
    { 0x0B0, 0x0F0 },                     // dma, dma fill
    { 0x100, 0x110 },                     // timers
    { 0x130, 0x138 },                     // keys
    { 0x180, 0x188 },                     // ipc sync, fifo control
    { 0x204, 0x20C }, { 0x210, 0x218 },   // exmemcnt, ime, ie, if
    { 0x240, 0x24C },                     // vramcnt, wramcnt
    { 0x280, 0x2C0 },                     // divider, square root
    { 0x300, 0x308 },                     // postflg, powcnt1
    { 0x600, 0x6A4 },                     // gxstat, ram count, 3d results
    { 0x1000, 0x1070 },                   // display B
};

// Before the menu changes anything.
void stateTakeIo(void)
{
    for (u32 i = 0; i < sizeof(sIoRanges) / sizeof(sIoRanges[0]); i++)
        for (u32 offset = sIoRanges[i][0]; offset < sIoRanges[i][1]; offset += 4)
            sIoDump[offset / 4] = *(vu32*)(0x04000000 + offset);
}

// ---------------------------------------------------------------- arm7 commands

static volatile rts_info_t* sRts;

static void command(u32 cmd, u32 sector, u32 address, u32 sectorCount)
{
    dcFlushAll(); // the arm7 reads memory as it is, and writes past the cache
    sRts->cmdSector = sector;
    sRts->cmdAddress = address;
    sRts->cmdSectorCount = sectorCount;
    sRts->menuState = cmd;
    dcFlushAll();
    for (;;)
    {
        for (vu32 i = 0; i < 0x400; i++); // off the bus while the arm7 works
        dcInvalidateLine(&sRts->menuState);
        if (sRts->menuState == RTS_MENU_STATE_DONE)
            break;
    }
    dcFlushAll(); // drop what the cache kept of memory the arm7 wrote
}

// Never anything outside the state file: offset and size are checked here.
static int writeState(u32 offset, u32 address, u32 size)
{
    if ((offset | size) & 511 || offset + size > sRts->stateSectorCount * 512 || offset + size < offset)
        return 0;
    command(RTS_MENU_STATE_SDWRITE, sRts->stateSector + offset / 512, address, size / 512);
    return 1;
}

static void copyWords(volatile u32* dst, const volatile u32* src, u32 bytes)
{
    for (u32 i = 0; i < bytes / 4; i++)
        dst[i] = src[i];
}

// From memory only the arm9 sees, in scratch sized steps.
static int writeStateFromArm9(u32 offset, const volatile void* src, u32 size)
{
    for (u32 done = 0; done < size; done += SCRATCH_SIZE)
    {
        u32 step = size - done < SCRATCH_SIZE ? size - done : SCRATCH_SIZE;
        copyWords((u32*)__scratch, (const volatile u32*)((u32)src + done), step);
        if (!writeState(offset + done, (u32)__scratch, step))
            return 0;
    }
    return 1;
}

// ---------------------------------------------------------------- the parts

static const struct
{
    u8 cnt;     // offset of its VRAMCNT from 0x04000240
    u32 lcdc;   // where lcdc mode puts it
    u32 size;
} sBanks[] =
{
    { 0, 0x06800000, 0x20000 }, // A: the menu's framebuffer; the game's copy is __vram_copy
    { 1, 0x06820000, 0x20000 },
    { 2, 0x06840000, 0x20000 },
    { 3, 0x06860000, 0x20000 },
    { 4, 0x06880000, 0x10000 },
    { 5, 0x06890000, 0x4000 },
    { 6, 0x06894000, 0x4000 },
    { 8, 0x06898000, 0x8000 },
    { 9, 0x068A0000, 0x4000 },
};

static int writeVram(void)
{
    u32 offset = STATE_VRAM;
    for (u32 i = 0; i < sizeof(sBanks) / sizeof(sBanks[0]); i++)
    {
        int ok;
        if (i == 0)
        {
            ok = writeState(offset, (u32)__vram_copy, sBanks[0].size);
        }
        else
        {
            vu8* cnt = (vu8*)(0x04000240 + sBanks[i].cnt);
            *cnt = 0x80; // enabled, lcdc
            ok = writeStateFromArm9(offset, (const volatile void*)sBanks[i].lcdc, sBanks[i].size);
            *cnt = gGameScreen.vramcnt[sBanks[i].cnt];
        }
        if (!ok)
            return 0;
        offset += sBanks[i].size;
    }
    return 1;
}

static int writeSharedWram(void)
{
    // WRAMCNT 0: all arm9; 1: first half arm7, second arm9; 2: the other way
    // round; 3: all arm7. A cpu sees its part mirrored from 0x037F8000.
    u32 mode = gGameScreen.vramcnt[7] & 3;
    for (u32 half = 0; half < 2; half++)
    {
        int arm7 = mode == 3 || (mode == 1 && half == 0) || (mode == 2 && half == 1);
        u32 offset = STATE_SHARED_WRAM + half * 0x4000;
        // a cpu with all 32 KB sees it in order; with one half, that half at the start
        u32 src = 0x037F8000 + (mode == 0 || mode == 3 ? half * 0x4000 : 0);
        int ok = arm7 ? writeState(offset, src, 0x4000)
                      : writeStateFromArm9(offset, (const volatile void*)src, 0x4000);
        if (!ok)
            return 0;
    }
    return 1;
}

static u32 cp15Read(int which)
{
    u32 v = 0;
    switch (which)
    {
        case 0: asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(v)); break;
        case 1: asm volatile("mrc p15, 0, %0, c9, c1, 0" : "=r"(v)); break;
        case 2: asm volatile("mrc p15, 0, %0, c9, c1, 1" : "=r"(v)); break;
        case 3: asm volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(v)); break;
        case 4: asm volatile("mrc p15, 0, %0, c2, c0, 1" : "=r"(v)); break;
        case 5: asm volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(v)); break;
        case 6: asm volatile("mrc p15, 0, %0, c5, c0, 2" : "=r"(v)); break;
        case 7: asm volatile("mrc p15, 0, %0, c5, c0, 3" : "=r"(v)); break;
        case 8: asm volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(v)); break;
        case 9: asm volatile("mrc p15, 0, %0, c6, c1, 0" : "=r"(v)); break;
        case 10: asm volatile("mrc p15, 0, %0, c6, c2, 0" : "=r"(v)); break;
        case 11: asm volatile("mrc p15, 0, %0, c6, c3, 0" : "=r"(v)); break;
        case 12: asm volatile("mrc p15, 0, %0, c6, c4, 0" : "=r"(v)); break;
        case 13: asm volatile("mrc p15, 0, %0, c6, c5, 0" : "=r"(v)); break;
        case 14: asm volatile("mrc p15, 0, %0, c6, c6, 0" : "=r"(v)); break;
        case 15: asm volatile("mrc p15, 0, %0, c6, c7, 0" : "=r"(v)); break;
    }
    return v;
}

static void buildHeader(state_header_t* h, void* block)
{
    memset(h, 0, 512);
    const u8* cart = (const u8*)block + HEADER_FROM_BLOCK;
    h->magic = STATE_MAGIC;
    h->version = STATE_VERSION;
    h->headerSize = sizeof(state_header_t);
    memcpy(h->title, cart, 12);
    h->gameCode = cart[12] | (cart[13] << 8) | (cart[14] << 16) | ((u32)cart[15] << 24);
    h->blockAddress = (u32)block;

    h->waitEntry = gSavedRegs[0];
    for (int i = 0; i < 8; i++)
        h->r4_r11[i] = gSavedRegs[2 + i];
    h->irqLr = gSavedRegs[10];
    h->irqSp = gSavedRegs[11];
    u32 banked[1 + 3 + 3 + 3 + 2 + 8];
    readBankedRegs(banked);
    h->irqSpsr = banked[0];
    memcpy(h->svc, &banked[1], 3 * 4);
    memcpy(h->abt, &banked[4], 3 * 4);
    memcpy(h->und, &banked[7], 3 * 4);
    memcpy(h->sys, &banked[10], 2 * 4);
    memcpy(h->fiq, &banked[12], 8 * 4);

    h->control = cp15Read(0);
    h->dtcmRegion = cp15Read(1);
    h->itcmRegion = cp15Read(2);
    h->dataCacheable = cp15Read(3);
    h->insnCacheable = cp15Read(4);
    h->writeBufferable = cp15Read(5);
    h->dataPermissions = cp15Read(6);
    h->insnPermissions = cp15Read(7);
    for (int i = 0; i < 8; i++)
        h->regions[i] = cp15Read(8 + i);

    memcpy(h->vramcnt, gGameScreen.vramcnt, 10);
    h->powcnt1 = gGameScreen.powcnt1;
    h->dispcntA = gGameScreen.dispcntA;
    h->dispcntB = gGameScreen.dispcntB;
    h->dispcapcnt = gGameScreen.dispcapcnt;
    h->masterBrightA = gGameScreen.masterBrightA;
    h->masterBrightB = gGameScreen.masterBrightB;
    h->backdropB = gGameScreen.backdropB;
    h->stashBase = RTS_BASE;
    h->stashSize = RTS_STASH_SIZE;
}

// ---------------------------------------------------------------- save

#define MAIN_STEP 0x40000

const char* stateSave(void* block, void (*progress)(u32 done, u32 total))
{
    sRts = (volatile rts_info_t*)((u8*)block + RTS_INFO_OFFSET);
    if (sRts->stateSector == 0 || sRts->stateSectorCount * 512 < STATE_END)
        return "no state file";

    // ten single steps, main ram in MAIN_STEP parts around the stash in scratch parts
    const u32 total = 10 + 0x100000 / MAIN_STEP + (0x400000 - 0x100000 - RTS_STASH_SIZE + MAIN_STEP - 1) / MAIN_STEP
        + RTS_STASH_SIZE / SCRATCH_SIZE;
    u32 done = 0;
#define STEP(x) do { if (!(x)) return "write refused"; progress(++done, total); } while (0)

    // no valid header while the rest is being written
    memset(__scratch, 0, 512);
    STEP(writeState(STATE_HEADER, (u32)__scratch, 512));

    // the game's register values where the menu changed them
    u32* io = (u32*)__scratch;
    memset(io, 0, STATE_ARM7_IO - STATE_ARM9_IO);
    copyWords(io, sIoDump, STATE_ARM9_IO_SIZE);
    STEP(writeState(STATE_ARM9_IO, (u32)io, STATE_ARM7_IO - STATE_ARM9_IO)); // in whole sectors

    STEP(writeStateFromArm9(STATE_ITCM, (const volatile void*)0x01FF8000, 0x8000));
    u32 dtcm = cp15Read(1) & ~0xFFF;
    STEP(writeStateFromArm9(STATE_DTCM, (const volatile void*)dtcm, 0x4000));

    // palettes, with the game's sub backdrop
    copyWords((u32*)__scratch, (const volatile u32*)0x05000000, 0x800);
    ((u16*)__scratch)[0x200] = gGameScreen.backdropB;
    STEP(writeState(STATE_PALETTE, (u32)__scratch, 0x800));
    STEP(writeStateFromArm9(STATE_OAM, (const volatile void*)0x07000000, 0x800));

    STEP(writeSharedWram());
    STEP(writeState(STATE_ARM7_WRAM, 0x03800000, 0x10000));
    STEP(writeVram());

    // main ram: the part under the menu is in the stash file
    u32 stashStart = RTS_BASE - 0x02000000; // 0x100000, a multiple of MAIN_STEP
    for (u32 offset = 0; offset < 0x400000; )
    {
        if (offset == stashStart)
        {
            for (u32 part = 0; part < RTS_STASH_SIZE; part += SCRATCH_SIZE)
            {
                command(RTS_MENU_STATE_SDREAD, sRts->stashSector + part / 512, (u32)__scratch, SCRATCH_SIZE / 512);
                STEP(writeState(STATE_MAIN_RAM + offset + part, (u32)__scratch, SCRATCH_SIZE));
            }
            offset += RTS_STASH_SIZE;
            continue;
        }
        u32 step = MAIN_STEP;
        if (offset < stashStart && offset + step > stashStart)
            step = stashStart - offset;
        if (offset + step > 0x400000)
            step = 0x400000 - offset;
        STEP(writeState(STATE_MAIN_RAM + offset, 0x02000000 + offset, step));
        offset += step;
    }

    buildHeader((state_header_t*)__scratch, block);
    STEP(writeState(STATE_HEADER, (u32)__scratch, 512));
    while (done < total)
        progress(++done, total);
    return 0;
#undef STEP
}
