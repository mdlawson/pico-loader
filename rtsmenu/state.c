// Writing a save state (state.h) while the game is frozen. The arm7 does the sd
// card side through the menu command channel; what only the arm9 can read
// (tcms, vram banks, palettes, oam, registers) goes through __scratch first.

#include <string.h>
#include "rtsmenu.h"
#include "state.h"
#include "stateint.h"

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

// ---------------------------------------------------------------- arm9 timers

// The game's timers stop while the menu runs and go on from the same count when
// it leaves, after a load from the state's. Their reloads can't be read: the
// menu measures each by waiting for the timer to wrap and reading the count it
// starts again from. Timers it can't measure (cascaded, or counting every bus
// cycle, too fast to catch the wrap) keep running as before.
#define REG_TMCNT(i)    ((vu16*)(0x04000100 + (i) * 4)) // [0] count / reload, [1] control
#define REG_IF          (*(vu32*)0x04000214)
#define REG_VCOUNT      (*(vu16*)0x04000006)
#define IF_TIMERS       0x78

static u32 sTimer[4];       // count | control << 16, as the menu found them
static u16 sTimerReload[4];
static u32 sTimerMeasured;  // bit per timer: stopped for the menu, reload known
static u32 sTimerIf;        // IF's timer bits as the menu found them

// The loaded state's, applied as the menu leaves.
static u32 sLoadTimer[4];
static u16 sLoadTimerReload[4];
static u32 sLoadTimerMeasured;

static int measureReload(int i, u16* reload)
{
    vu16* t = REG_TMCNT(i);
    // a period in scanlines (2130 bus cycles), at 64, 256 or 1024 cycles a tick
    static const u32 periodLines[4] = { 0, 1970, 7880, 31510 };
    u32 limit = periodLines[t[1] & 3] + 300;
    u32 lines = 0, n = 0;
    u16 line = REG_VCOUNT;
    u16 prev = t[0];
    for (;;)
    {
        u16 v = t[0];
        if (v < prev)
        {
            *reload = v;
            return 1;
        }
        prev = v;
        if ((++n & 255) == 0)
        {
            u16 l = REG_VCOUNT;
            lines += (l + 263 - line) % 263;
            line = l;
            if (lines > limit)
                return 0;
        }
    }
}

static void timersStop(void)
{
    sTimerIf = REG_IF & IF_TIMERS;
    for (int i = 0; i < 4; i++)
        sTimer[i] = *(vu32*)(0x04000100 + i * 4);
    sTimerMeasured = 0;
    for (int i = 0; i < 4; i++)
    {
        u16 control = sTimer[i] >> 16;
        if (!(control & 0x80) || (control & 4) || (control & 3) == 0)
            continue;
        if (measureReload(i, &sTimerReload[i]))
            sTimerMeasured |= 1u << i;
    }
    for (int i = 0; i < 4; i++)
        if (sTimerMeasured & (1u << i))
            REG_TMCNT(i)[1] = (sTimer[i] >> 16) & ~0x80;
}

static void timerStart(int i, u32 timer, u16 reload)
{
    vu16* t = REG_TMCNT(i);
    t[1] = (timer >> 16) & ~0x80;
    t[0] = (u16)timer;
    t[1] = timer >> 16; // counts on from there
    t[0] = reload;      // and wraps to the reload
}

static void timersLeave(int loaded)
{
    // the overflows the measuring caused are not the game's
    REG_IF = REG_IF & IF_TIMERS & ~sTimerIf & (sTimerMeasured << 3);
    for (int i = 0; i < 4; i++)
    {
        if (!loaded)
        {
            if (sTimerMeasured & (1u << i))
                timerStart(i, sTimer[i], sTimerReload[i]);
        }
        else if (sLoadTimerMeasured & (1u << i))
            timerStart(i, sLoadTimer[i], sLoadTimerReload[i]);
        else
        {
            // reload unknown: the count as the reload, then the control
            vu16* t = REG_TMCNT(i);
            if (sTimerMeasured & (1u << i))
                t[1] = (sLoadTimer[i] >> 16) & ~0x80; // stopped by the menu: starts again below
            t[0] = (u16)sLoadTimer[i];
            t[1] = sLoadTimer[i] >> 16;
        }
    }
}

// ---------------------------------------------------------------- 3d matrices

// The geometry engine's matrices are write only, but the clip matrix (position
// x projection) and the vector matrix can be read. With the other one set to
// identity for a moment (pushed and popped around it), the clip matrix is the
// projection or the position matrix alone. Games that set their projection once
// per scene need it back after a load. The matrix mode is found from which
// stack level a push moves.
#define REG_GXSTAT      (*(vu32*)0x04000600)
#define REG_CLIPMTX     ((vs32*)0x04000640)
#define REG_VECMTX      ((vs32*)0x04000680)
#define GX_MTX_MODE     (*(vu32*)0x04000440)
#define GX_MTX_PUSH     (*(vu32*)0x04000444)
#define GX_MTX_POP      (*(vu32*)0x04000448)
#define GX_MTX_IDENTITY (*(vu32*)0x04000454)
#define GX_MTX_LOAD44   (*(vu32*)0x04000458)
#define GXSTAT_BUSY     ((1u << 27) | (1u << 14)) // geometry engine, matrix stack
#define GXSTAT_ERROR    (1u << 15)
#define POS_LEVEL(st)   (((st) >> 8) & 0x1F)
#define PROJ_LEVEL(st)  (((st) >> 13) & 1)
typedef int32_t s32;
typedef volatile int32_t vs32;

typedef struct
{
    s32 proj[16], pos[16], vec[9];
    u32 mode, stat, valid;
} gx_t;
static gx_t sGx;        // as the menu found them
static gx_t sLoadGx;    // the loaded state's

static int gxIdle(void)
{
    for (u32 i = 0; i < 0x100000; i++)
        if (!(REG_GXSTAT & GXSTAT_BUSY))
            return 1;
    return 0;
}

static void readClip(s32* out)
{
    gxIdle();
    for (int i = 0; i < 16; i++)
        out[i] = REG_CLIPMTX[i];
}

static void gxSave(void)
{
    sGx.valid = 0;
    // only between frames' commands: nothing queued or running
    if (!gxIdle() || !(REG_GXSTAT & (1u << 26)))
        return;
    u32 st = REG_GXSTAT;
    sGx.stat = st;
    if (st & GXSTAT_ERROR)
        return;
    // the mode: a push moves the projection level (0), the position level (1 or
    // 2) or neither (3, the texture stack); a full stack says so with its error
    GX_MTX_PUSH = 0;
    gxIdle();
    u32 after = REG_GXSTAT;
    u32 mode;
    if (PROJ_LEVEL(after) != PROJ_LEVEL(st) || ((after & GXSTAT_ERROR) && PROJ_LEVEL(st)))
        mode = 0;
    else if (POS_LEVEL(after) != POS_LEVEL(st) || ((after & GXSTAT_ERROR) && POS_LEVEL(st) == 31))
        mode = 2;
    else
        mode = 3;
    if (!(after & GXSTAT_ERROR))
        GX_MTX_POP = 1;
    REG_GXSTAT = GXSTAT_ERROR; // acknowledged
    for (int i = 0; i < 9; i++)
        sGx.vec[i] = REG_VECMTX[i];
    if (mode == 2)
    {
        // 1 or 2: whether identity in this mode changes the vector matrix too
        GX_MTX_PUSH = 0;
        GX_MTX_IDENTITY = 0;
        gxIdle();
        for (int i = 0; i < 9; i++)
            if (REG_VECMTX[i] != sGx.vec[i])
                break;
            else if (i == 8)
                mode = 1; // unchanged (or it was identity already: either does)
        GX_MTX_POP = 1;
    }
    sGx.mode = mode;
    // the projection: clip with the position matrix at identity
    if (POS_LEVEL(st) >= 31)
        return;
    GX_MTX_MODE = 1;
    GX_MTX_PUSH = 0;
    GX_MTX_IDENTITY = 0;
    readClip(sGx.proj);
    GX_MTX_POP = 1;
    // the position matrix: clip with the projection at identity, then the
    // projection back from what was just read
    GX_MTX_MODE = 0;
    GX_MTX_IDENTITY = 0;
    readClip(sGx.pos);
    for (int i = 0; i < 16; i++)
        GX_MTX_LOAD44 = sGx.proj[i];
    GX_MTX_MODE = sGx.mode;
    gxIdle();
    sGx.valid = !(REG_GXSTAT & GXSTAT_ERROR);
    REG_GXSTAT = GXSTAT_ERROR;
}

static void gxLoad(const gx_t* g)
{
    if (!g->valid || !gxIdle())
        return;
    // the stack levels: what the state had pushed, with these matrices (the
    // pushed ones themselves can't be read)
    GX_MTX_MODE = 0;
    for (int i = 0; i < 16; i++)
        GX_MTX_LOAD44 = g->proj[i];
    GX_MTX_MODE = 2; // position and vector: the vector one first, as a 4x4
    static const u8 vec44[16] = { 0, 1, 2, 16, 3, 4, 5, 16, 6, 7, 8, 16, 16, 16, 16, 17 };
    for (int i = 0; i < 16; i++)
        GX_MTX_LOAD44 = vec44[i] == 16 ? 0 : vec44[i] == 17 ? 0x1000 : g->vec[vec44[i]];
    GX_MTX_MODE = 1;
    for (int i = 0; i < 16; i++)
        GX_MTX_LOAD44 = g->pos[i];
    gxIdle();
    u32 now = REG_GXSTAT;
    GX_MTX_MODE = 2;
    for (u32 l = POS_LEVEL(now); l > POS_LEVEL(g->stat); l--)
        GX_MTX_POP = 1;
    for (u32 l = POS_LEVEL(now); l < POS_LEVEL(g->stat); l++)
        GX_MTX_PUSH = 0;
    GX_MTX_MODE = 0;
    if (PROJ_LEVEL(now) && !PROJ_LEVEL(g->stat))
        GX_MTX_POP = 1;
    else if (!PROJ_LEVEL(now) && PROJ_LEVEL(g->stat))
        GX_MTX_PUSH = 0;
    GX_MTX_MODE = g->mode;
    gxIdle();
    REG_GXSTAT = GXSTAT_ERROR;
}

// Before the menu changes anything.
void stateTakeIo(void)
{
    timersStop();
    gxSave();
    for (u32 i = 0; i < sizeof(sIoRanges) / sizeof(sIoRanges[0]); i++)
        for (u32 offset = sIoRanges[i][0]; offset < sIoRanges[i][1]; offset += 4)
            sIoDump[offset / 4] = *(vu32*)(0x04000000 + offset);
    // the timers as the game had them, not as the menu stopped them
    for (int i = 0; i < 4; i++)
        sIoDump[0x100 / 4 + i] = sTimer[i];
}

// ---------------------------------------------------------------- arm7 commands

volatile rts_info_t* sRts;

static u32 sSlotSector; // the slot the next reads and writes go to

u32 slotSector(int slot)
{
    if (slot == 1)
        return sRts->stateSector;
    if (slot == 2 || slot == 3)
        return sRts->testSlotSector[slot - 2];
    return 0;
}

// Returns the arm7's answer: RTS_MENU_STATE_DONE, or _FAIL.
u32 command(u32 cmd, u32 sector, u32 address, u32 sectorCount)
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
        if (sRts->menuState == RTS_MENU_STATE_DONE || sRts->menuState == RTS_MENU_STATE_FAIL)
            break;
    }
    dcFlushAll(); // drop what the cache kept of memory the arm7 wrote
    return sRts->menuState;
}

// The zero map of the slot sZeroSlot (0: none read): a bit per 2 KB block of
// its file, set when the block was all zero and the save skipped it. Writes are
// one sector per card command, so this is most of what makes saving quicker.
static u32 sZero[STATE_ZERO_MAP_SIZE / 4];
static int sZeroSlot;
static int sZeroValid;

static int zeroBlock(const volatile u32* p)
{
    u32 any = 0;
    for (u32 i = 0; i < STATE_ZERO_BLOCK / 4; i += 4)
        any |= p[i] | p[i + 1] | p[i + 2] | p[i + 3];
    return any == 0;
}

// Never anything outside the state file: offset and size are checked here.
// From STATE_VRAM on, while a save keeps a zero map, the all-zero blocks of
// memory the arm9 can read are left out and marked instead.
static int writeState(u32 offset, u32 address, u32 size)
{
    if ((offset | size) & 511 || offset + size > STATE_END || offset + size < offset || !sSlotSector)
        return 0;
    if (!sZeroValid || offset + size <= STATE_VRAM)
    {
        command(RTS_MENU_STATE_SDWRITE, sSlotSector + offset / 512, address, size / 512);
        return 1;
    }
    // whole blocks only here (all the saves are), so no block is half written
    if ((offset | size) & (STATE_ZERO_BLOCK - 1) || offset < STATE_VRAM)
        return 0;
    int arm9 = address >= 0x02000000 && address < 0x02400000;
    const u32 per = STATE_ZERO_BLOCK / 512;
    u32 first = offset / STATE_ZERO_BLOCK, count = size / STATE_ZERO_BLOCK, run = 0;
    for (u32 b = 0; b <= count; b++)
    {
        int zero = b < count && arm9 && zeroBlock((const volatile u32*)(address + b * STATE_ZERO_BLOCK));
        if (b < count)
        {
            u32 bit = 1u << ((first + b) & 31);
            if (zero)
                sZero[(first + b) >> 5] |= bit;
            else
                sZero[(first + b) >> 5] &= ~bit;
        }
        if (b < count && !zero)
        {
            run++;
            continue;
        }
        if (run)
            command(RTS_MENU_STATE_SDWRITE, sSlotSector + (first + b - run) * per,
                address + (b - run) * STATE_ZERO_BLOCK, run * per);
        run = 0;
    }
    return 1;
}

void copyWords(volatile u32* dst, const volatile u32* src, u32 bytes)
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

const vram_bank_t sBanks[9] =
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

u32 cp15Read(int which)
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

// ---------------------------------------------------------------- ipc fifos

#define REG_IPCFIFOCNT  (*(vu16*)0x04000184)
#define REG_IPCFIFOSEND (*(vu32*)0x04000188)
#define REG_IPCFIFORECV (*(vu32*)0x04100000)
#define FIFOCNT_WRITABLE 0x8404 // enable, receive and send irq enables

// count, then the words: what the arm9's receive fifo held on entry, and
// what the arm7 says its receive fifo held
static u32 sFifo9[17];
static u32 sFifo7[17];
static int sLoaded;

void stateEnter(void* block)
{
    sRts = (volatile rts_info_t*)((u8*)block + RTS_INFO_OFFSET);
    u16 cnt = REG_IPCFIFOCNT;
    REG_IPCFIFOCNT = cnt | 0x8000; // enabled to read
    u32 n = 0;
    while (n < 16 && !(REG_IPCFIFOCNT & 0x100)) // until receive empty
        sFifo9[1 + n++] = REG_IPCFIFORECV;
    sFifo9[0] = n;
    REG_IPCFIFOCNT = (cnt & FIFOCNT_WRITABLE) | 0x4000; // error acknowledged
}

static void fifoSendFromArm9(const u32* words, u32 count)
{
    u16 cnt = REG_IPCFIFOCNT;
    REG_IPCFIFOCNT = cnt | 0x8000;
    for (u32 i = 0; i < count && i < 16 && !(REG_IPCFIFOCNT & 2); i++) // not send full
        REG_IPCFIFOSEND = words[i];
    REG_IPCFIFOCNT = (cnt & FIFOCNT_WRITABLE) | 0x4000;
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
    h->fifo9Count = sFifo9[0];
    memcpy(h->fifo9, &sFifo9[1], 16 * 4);
    h->fifo7Count = sFifo7[0] <= 16 ? sFifo7[0] : 0;
    memcpy(h->fifo7, &sFifo7[1], 16 * 4);
    memcpy(h->timer9, sTimer, sizeof(sTimer));
    memcpy(h->timerReload9, sTimerReload, sizeof(sTimerReload));
    h->timerMeasured9 = sTimerMeasured;
}

// ---------------------------------------------------------------- save

#define MAIN_STEP 0x40000

static const char* save(void* block, int slot, void (*progress)(u32 done, u32 total))
{
    sRts = (volatile rts_info_t*)((u8*)block + RTS_INFO_OFFSET);
    sSlotSector = slotSector(slot);
    if (sSlotSector == 0)
        return "no state file";
    // a fresh zero map, kept as the slot's while it is written
    memset(sZero, 0, sizeof(sZero));
    sZeroSlot = slot;
    sZeroValid = 1;

    // eleven single steps, main ram in MAIN_STEP parts around the stash in scratch parts
    const u32 total = 11 + 0x100000 / MAIN_STEP + (0x400000 - 0x100000 - RTS_STASH_SIZE + MAIN_STEP - 1) / MAIN_STEP
        + RTS_STASH_SIZE / SCRATCH_SIZE;
    u32 done = 0;
#define STEP(x) do { if (!(x)) { sZeroSlot = 0; sZeroValid = 0; return "write refused"; } progress(++done, total); } while (0)

    // no valid header while the rest is being written
    memset(__scratch, 0, 512);
    STEP(writeState(STATE_HEADER, (u32)__scratch, 512));

    // the game's register values where the menu changed them
    u32* io = (u32*)__scratch;
    memset(io, 0, STATE_ARM7_IO - STATE_ARM9_IO);
    copyWords(io, sIoDump, STATE_ARM9_IO_SIZE);
    STEP(writeState(STATE_ARM9_IO, (u32)io, STATE_ARM7_IO - STATE_ARM9_IO)); // in whole sectors

    // the arm7's registers and what its stub kept of the game's
    memset(__scratch, 0, STATE_ITCM - STATE_ARM7_IO);
    command(RTS_MENU_STATE_ARM7_SAVE, 0, (u32)__scratch, 0);
    {
        state_extra_t* x = (state_extra_t*)(__scratch + (STATE_EXTRA - STATE_ARM7_IO));
        x->magic = STATE_EXTRA_MAGIC;
        memcpy(x->gxProj, sGx.proj, sizeof(sGx.proj));
        memcpy(x->gxPos, sGx.pos, sizeof(sGx.pos));
        memcpy(x->gxVec, sGx.vec, sizeof(sGx.vec));
        x->gxMode = sGx.mode;
        x->gxStat = sGx.stat;
        x->gxValid = sGx.valid;
    }
    STEP(writeState(STATE_ARM7_IO, (u32)__scratch, STATE_ITCM - STATE_ARM7_IO));

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

    command(RTS_MENU_STATE_FIFO_QUERY, 0, (u32)sFifo7, 0);
    STEP(writeState(STATE_ZERO_MAP, (u32)sZero, STATE_ZERO_MAP_SIZE));
    buildHeader((state_header_t*)__scratch, block);
    ((state_header_t*)__scratch)->zeroMap = 1;
    STEP(writeState(STATE_HEADER, (u32)__scratch, 512));
    while (done < total)
        progress(++done, total);
    return 0;
#undef STEP
}

// ---------------------------------------------------------------- load

static state_header_t sHeader;

// The slot's zero map, from its header (a slot written by this menu since has
// its own in sZero already).
static int zeroMapFor(int slot, u32 sector)
{
    if (sZeroSlot == slot)
        return 1;
    sZeroSlot = 0;
    sZeroValid = 0;
    if (command(RTS_MENU_STATE_SDREAD, sector + STATE_HEADER / 512, (u32)sZero, 1) != RTS_MENU_STATE_DONE)
        return 0;
    const state_header_t* h = (const state_header_t*)sZero;
    int mapped = h->magic == STATE_MAGIC && h->version >= 5 && h->zeroMap == 1;
    if (mapped && command(RTS_MENU_STATE_SDREAD, sector + STATE_ZERO_MAP / 512, (u32)sZero,
            STATE_ZERO_MAP_SIZE / 512) != RTS_MENU_STATE_DONE)
        return 0;
    sZeroSlot = slot;
    sZeroValid = mapped;
    return 1;
}

int readSlot(int slot, u32 offset, u32 address, u32 size)
{
    u32 sector = slotSector(slot);
    if ((offset | size) & 511 || offset + size > STATE_END || offset + size < offset || !sector)
        return 0;
    if (offset + size <= STATE_VRAM)
        return command(RTS_MENU_STATE_SDREAD, sector + offset / 512, address, size / 512)
            == RTS_MENU_STATE_DONE;
    if (!zeroMapFor(slot, sector))
        return 0;
    // the skipped sectors are zero, filled here: memory the arm9 can reach
    // only (main memory, the scratch and vram copies are)
    int arm9 = address >= 0x02000000 && address < 0x02400000;
    u32 first = offset / 512, count = size / 512, run = 0, filled = 0;
    for (u32 s = 0; s <= count; s++)
    {
        u32 block = (first + s) / (STATE_ZERO_BLOCK / 512);
        int zero = s < count && sZeroValid && first + s >= STATE_VRAM / 512
            && (sZero[block >> 5] >> (block & 31) & 1);
        if (zero && !arm9)
            return 0; // nothing to fill it with: the file holds stale sectors there
        if (s < count && !zero)
        {
            run++;
            continue;
        }
        if (run && command(RTS_MENU_STATE_SDREAD, sector + first + s - run, address + (s - run) * 512, run)
            != RTS_MENU_STATE_DONE)
            return 0;
        run = 0;
        if (zero)
        {
            vu32* p = (vu32*)(address + s * 512);
            for (u32 i = 0; i < 128; i++)
                p[i] = 0;
            filled = 1;
        }
    }
    if (filled)
        dcFlushAll(); // the next arm7 command reads memory as it is
    return 1;
}

static int readState(u32 offset, u32 address, u32 size)
{
    return readSlot(1, offset, address, size);
}

// Into memory only the arm9 sees, in scratch sized steps.
static int readStateToArm9(u32 offset, volatile void* dst, u32 size)
{
    for (u32 done = 0; done < size; done += SCRATCH_SIZE)
    {
        u32 step = size - done < SCRATCH_SIZE ? size - done : SCRATCH_SIZE;
        if (!readState(offset + done, (u32)__scratch, step))
            return 0;
        copyWords((volatile u32*)((u32)dst + done), (const u32*)__scratch, step);
    }
    return 1;
}

// The stash file gets the state's main ram under the menu: the arm7 puts it in
// place when the menu leaves. Never anything outside the stash itself.
static int writeStash(u32 offset, u32 address, u32 size)
{
    if ((offset | size) & 511 || offset + size > RTS_STASH_SIZE)
        return 0;
    return command(RTS_MENU_STATE_SDWRITE, sRts->stashSector + offset / 512, address, size / 512)
        == RTS_MENU_STATE_DONE;
}

const char* stateCheck(void* block)
{
    sRts = (volatile rts_info_t*)((u8*)block + RTS_INFO_OFFSET);
    if (sRts->stateSector == 0)
        return "no state file";
    if (!readState(STATE_HEADER, (u32)__scratch, 512))
        return "could not read it";
    memcpy(&sHeader, __scratch, sizeof(sHeader));
    const u8* cart = (const u8*)block + HEADER_FROM_BLOCK;
    u32 gameCode = cart[12] | (cart[13] << 8) | (cart[14] << 16) | ((u32)cart[15] << 24);
    if (sHeader.magic != STATE_MAGIC)
        return "slot 1 is empty";
    if (sHeader.version < 2 || sHeader.version > STATE_VERSION)
        return "older format, save again";
    if (sHeader.gameCode != gameCode)
        return "from another game";
    // the same patches in the same places, entered the same way
    if (sHeader.blockAddress != (u32)block || sHeader.waitEntry != gSavedRegs[0] ||
        sHeader.irqLr != gSavedRegs[10] || sHeader.irqSp != gSavedRegs[11])
        return "other loader setup (arm9)";
    if (!readState(STATE_ARM7_IO, (u32)__scratch, 512))
        return "could not read it";
    if (command(RTS_MENU_STATE_ARM7_LOAD, 0, (u32)__scratch, 0) != RTS_MENU_STATE_DONE)
        return "other loader setup (arm7)";
    return 0;
}

// One sector of main ram, all but [keepStart, keepEnd) (offsets from 0x02000000).
static int restoreSectorAround(u32 offset, u32 keepStart, u32 keepEnd)
{
    if (!readState(STATE_MAIN_RAM + offset, (u32)__scratch, 512))
        return 0;
    vu8* ram = (vu8*)(0x02000000 + offset);
    for (u32 i = 0; i < 512; i++)
        if (offset + i < keepStart || offset + i >= keepEnd)
            ram[i] = __scratch[i];
    dcFlushAll();
    return 1;
}

// The writable arm9 registers, from the dump (offsets from 0x04000000, size),
// in this order. The scroll, affine, window size, mosaic and fade registers
// can't be read, so they stay as the running session left them.
static const u16 sIoRestore[][2] =
{
    { 0x004, 2 },                                               // DISPSTAT: vcount line and irq enables
    { 0x008, 8 }, { 0x048, 4 }, { 0x050, 2 }, { 0x060, 4 },     // BGxCNT, WININ/OUT, BLDCNT, DISP3DCNT
    { 0x1008, 8 }, { 0x1048, 4 }, { 0x1050, 2 },                // the same of engine B
    { 0x0E0, 16 },                                              // dma fill values
    { 0x280, 2 }, { 0x290, 16 },                                // DIVCNT, numerator, denominator: computes again
    { 0x2B0, 2 }, { 0x2B8, 8 },                                 // SQRTCNT, parameter: the same
    { 0x210, 4 },                                               // IE
};

#define MAIN_STEP_DO(x) do { if (progress) { if (!(x)) return 0; progress(++*done, total); } else ++*done; } while (0)
// Main ram around the keep ranges (offsets from 0x02000000): with progress 0
// it only counts the steps. Returns 0 when a read failed.
static int restoreMainRam(u32 keep[KEEP_RANGES][2], void (*progress)(u32 done, u32 total), u32* done, u32 total)
{
    for (u32 ram = 0; ram < 0x400000; )
    {
        u32 step = MAIN_STEP - (ram % MAIN_STEP);
        int around = -1;
        for (int k = 0; k < KEEP_RANGES; k++)
        {
            u32 start = keep[k][0] & ~511, end = (keep[k][1] + 511) & ~511;
            if (start == end)
                continue;
            if (ram >= start && ram < end)
                around = k;
            else if (ram < start && ram + step > start)
                step = start - ram;
        }
        if (around == 0)
        {
            ram = keep[0][1]; // the stash window is whole sectors
            continue;
        }
        if (around > 0)
        {
            MAIN_STEP_DO(restoreSectorAround(ram, keep[around][0], keep[around][1]));
            ram += 512;
            continue;
        }
        if (ram + step > 0x400000)
            step = 0x400000 - ram;
        MAIN_STEP_DO(readState(STATE_MAIN_RAM + ram, 0x02000000 + ram, step));
        ram += step;
    }
    return 1;
}

int dmaRestored(u32 control)
{
    u32 timing = (control >> 27) & 7;
    return (control & (1u << 31)) && (control & (1u << 25)) && timing >= 1 && timing <= 4;
}

// The game's dma channels from a loaded state, set as the menu leaves (so
// none writes under the menu): source, destination, control per channel.
static u32 sDma[4][3];

// The census's shadow of the registers no one can read back (CensusPatchCode.s,
// from its arrays): the state brought its own along with its main memory, so
// after a load it holds the saved session's values; written back here. A state
// whose stubs belong to another build of the census gets its patched sites put
// back as they were instead, as its stubs would run the wrong code.
#define SH_MAGIC        0x31444853 // 'SHD1'
#define SH_SHADOW       0x000
#define SH_POOL         0x500
#define SH_DATA         8
#define SH_FLAGS        0x128

static void shadowLeave(void)
{
    u32 start = sRts->censusStart;
    if (start < 0x02000000 || start >= 0x02400000 || (start & 3))
        return;
    volatile u32* live = (volatile u32*)start;
    if (live[0] == 0x32444853 /* 'SHD2' */)
    {
        // the trap-free shadow (shadow/ShadowPatchCode.s): came with the state,
        // stubs, code and all
        const u8* data = (const u8*)start + 0x10;
        const u8* flags = (const u8*)start + 0x130;
        for (u32 i = 0; i < 0x120; i += 2)
        {
            if (!flags[i] && !flags[i + 1])
                continue;
            u32 addr = i < 0x48 ? 0x04000010 + i : i < 0x90 ? 0x04001010 + (i - 0x48) : 0x04000330 + (i - 0x90);
            *(vu16*)addr = data[i] | (data[i + 1] << 8);
        }
        return;
    }
    if (live[0x108 / 4] != 0x31534E43 /* 'CNS1' */)
        return;
    u8* arrays = (u8*)start + CENSUS_ARRAYS_OFFSET;
    u32* sh = (u32*)(arrays + SH_SHADOW);
    if (sh[0] != SH_MAGIC)
        return; // a state from before the shadow
    // the open routine of this build is in the live code, which a load keeps
    if (sh[1] != live[0x118 / 4])
    {
        // each stub: str lr / bl open / the store / bl close / ldr lr / b site+4
        u32 slots = live[0x11C / 4];
        for (u32 i = 0; i < slots && i < 512; i++)
        {
            u32* stub = (u32*)(arrays + SH_POOL + i * 24);
            if (stub[0] != 0xE52DE010 || (stub[5] >> 24) != 0xEA)
                continue;
            u32 site = (u32)&stub[5] + 8 + ((int)(stub[5] << 8) >> 6) - 4;
            u32 old = site >= 0x02000000 && site < 0x02400000 ? *(vu32*)site : 0;
            u32 branch = 0x0A000000 | (((u32)stub - (site + 8)) >> 2 & 0xFFFFFF);
            if ((old & 0x0FFFFFFF) == branch)
                *(vu32*)site = (old & 0xF0000000) | (stub[2] & 0x0FFFFFFF);
            stub[0] = 0;
        }
        sh[0] = 0;
        dcFlushAll();
        icInvalidateAll();
        return;
    }
    const u8* data = (const u8*)sh + SH_DATA;
    const u8* flags = (const u8*)sh + SH_FLAGS;
    for (u32 i = 0; i < 0x120; i += 2)
    {
        if (!flags[i] && !flags[i + 1])
            continue;
        u32 addr = i < 0x48 ? 0x04000010 + i : i < 0x90 ? 0x04001010 + (i - 0x48) : 0x04000330 + (i - 0x90);
        *(vu16*)addr = data[i] | (data[i + 1] << 8);
    }
}

void stateLeave(void)
{
    // The words each cpu had waiting go back the way they came: the arm7 sends
    // the arm9's, the arm9 the arm7's. Both fifos are empty now: each side
    // took its own on entry.
    if (!sLoaded)
        command(RTS_MENU_STATE_FIFO_QUERY, 0, (u32)sFifo7, 0);
    command(RTS_MENU_STATE_FIFO_SEND, 0, (u32)&sFifo9[1], sFifo9[0] <= 16 ? sFifo9[0] : 0);
    fifoSendFromArm9(&sFifo7[1], sFifo7[0] <= 16 ? sFifo7[0] : 0);

    if (!sLoaded)
    {
        timersLeave(0);
        return;
    }
    sLoaded = 0;
    for (int ch = 0; ch < 4; ch++)
    {
        vu32* dma = (vu32*)(0x040000B0 + ch * 12);
        dma[2] = 0; // stop what the running session had; the next enable reloads
        u32 control = sDma[ch][2];
        // only the repeating ones the display starts; one-shot transfers ended
        // long ago in the saved session, and card or 3d fifo ones need their source
        if (!dmaRestored(control))
        {
            // the game's values, left as a finished transfer leaves them
            dma[0] = sDma[ch][0];
            dma[1] = sDma[ch][1];
            dma[2] = control & ~(1u << 31);
            continue;
        }
        dma[0] = sDma[ch][0];
        dma[1] = sDma[ch][1];
        dma[2] = control;
    }
    gxLoad(&sLoadGx);
    shadowLeave();
    timersLeave(1);
    if (gCheckOnLeave)
    {
        gCheckOnLeave = 0;
        sRts->testResult = (sRts->testResult & 0xFFFF) | checkLoadedRegisters() << 16;
    }
}

void mainRamKeep(void* block, u32 keep[KEEP_RANGES][2])
{
    keep[0][0] = RTS_BASE - 0x02000000;
    keep[0][1] = RTS_BASE - 0x02000000 + RTS_STASH_SIZE;
    keep[1][0] = (u32)block & 0x3FFFFF;
    keep[1][1] = ((u32)block & 0x3FFFFF) + RTS_INFO_OFFSET + sizeof(rts_info_t);
    keep[2][0] = keep[2][1] = 0;
    if (sRts->arm7PatchStart >= 0x02000000 && sRts->arm7PatchStart < 0x03000000)
    {
        keep[2][0] = sRts->arm7PatchStart & 0x3FFFFF;
        keep[2][1] = keep[2][0] + sRts->arm7PatchSize;
    }
    // the census counts on through a load, and its variables say what the
    // protection unit holds now. Only its variables and code: its shadow, patch
    // table and stubs come with the state, as they go with the state's code.
    keep[3][0] = keep[3][1] = 0;
    if (sRts->censusStart >= 0x02000000 && sRts->censusStart < 0x03000000 && !(sRts->censusStart & 3)
        && ((volatile u32*)((sRts->censusStart & 0x3FFFFF) | 0x02000000))[0x108 / 4] == 0x31534E43 /* 'CNS1' */)
    {
        keep[3][0] = sRts->censusStart & 0x3FFFFF;
        keep[3][1] = keep[3][0] + CENSUS_ARRAYS_OFFSET;
    }
}

// The arm7 shadow (arm9/source/shadow/ShadowPatchCode7.s), version 3: at the
// end of the shadow space, which starts at censusStart.
#define S7_DATA         0x010
#define S7_FLAGS        0x130 // a bit per data byte
#define S7_FROZE_AT     0x19C
#define S7_HOLD         0x1A0
#define S7_DELAYED      0x1A4
#define S7_STARTED      0x1A8
#define S7_MAX_GAP      (263 * 60) // lines of silence a delayed start may leave: a second

// In the arm7 wram image (wram at 0x03800000; the loader puts it after the
// patch space there when it fits), else at the end of the main memory shadow
// space, which starts at censusStart.
static volatile u8* findShadow7(volatile u8* at, u32 size) // at: 32-byte aligned as the arm7 sees it
{
    for (u32 a = 0; a + 32 <= size; a += 32)
        if (*(vu32*)(at + a) == 0x37444853 /* 'SHD7' */ && *(vu32*)(at + a + 4) == 3)
            return at + a;
    return 0;
}

// A loaded state's pcm channels, from where they were: the hardware starts a
// channel at the top of its sample, so one that was before its loop (or in a
// one-shot) starts from the freeze's place in it, and one in its loop starts
// later, a loop or more after it began, when its top comes round to the time
// it had played (the arm7 shadow starts it then). The rest start from the top.
static void soundPlace(volatile u8* s7, const u32* cnt) // cnt: SOUNDxCNT as the freeze found them
{
    volatile u32* started = (volatile u32*)(s7 + S7_STARTED);
    u32 frozeAt = *(vu32*)(s7 + S7_FROZE_AT);
    u32 hold = 0, delayed = *(vu32*)(s7 + S7_DELAYED);
    for (u32 ch = 0; ch < 16; ch++)
    {
        u32 c = cnt[ch];
        if (!(c & 0x80000000))
            continue; // not playing: a start still to come stays
        delayed &= ~(1u << ch);
        u32 format = (c >> 29) & 3, repeat = (c >> 27) & 3;
        u16 flags = *(vu16*)(s7 + S7_FLAGS + ch * 2);
        u32 elapsed = frozeAt - started[ch];
        if (format >= 2 || (repeat != 1 && repeat != 2) || (flags & 0x1510) != 0x1510 // SAD, TMR, PNT, LEN
            || started[ch] == 0 || (s32)elapsed < 0)
            continue;
        volatile u8* d = s7 + S7_DATA + ch * 16;
        u32 rate = 0x10000 - *(vu16*)(d + 8); // a sample is 2 * rate cycles, a line 2130
        u32 unit = format == 1 ? 2 : 1;
        u64 bytes = (u64)elapsed * 1065 / rate * unit;
        u32 loopStart = *(vu16*)(d + 10) * 4, loopLength = (*(vu32*)(d + 12) & 0x3FFFFF) * 4;
        u32 end = loopStart + loopLength;
        if (bytes < loopStart || (repeat == 2 && bytes < end))
        {
            u32 off = (u32)bytes & ~3u;
            *(vu32*)(d + 4) += off;
            if (repeat == 1)
                *(vu16*)(d + 10) = (loopStart - off) / 4; // the loop where it was
            else
            {
                *(vu32*)(d + 12) = (end - off) / 4;
                *(vu16*)(d + 10) = 0;
            }
            started[ch] = frozeAt;
            continue;
        }
        if (repeat == 2)
        {
            hold |= 1u << ch; // a one-shot that ended
            continue;
        }
        if (loopLength == 0)
            continue;
        u32 pos = loopStart + (u32)((bytes - loopStart) % loopLength);
        // its top at frozeAt - pos + k * loopLength (in samples), the first
        // more than a frame on: the resume can wait up to one for the timers
        u64 gap = 0;
        for (u32 k = 1; k < 256 && gap < 265; k++)
            if (k * loopLength >= pos)
                gap = (u64)((k * loopLength - pos) / unit) * rate / 1065;
        if (gap < 265 || gap > S7_MAX_GAP)
            continue;
        started[ch] = frozeAt + (u32)gap;
        hold |= 1u << ch;
        delayed |= 1u << ch;
    }
    *(vu32*)(s7 + S7_HOLD) = hold;
    *(vu32*)(s7 + S7_DELAYED) = delayed;
    dcFlushAll();
}

static volatile u8* sShadow7Main; // where to place them after main ram is back
static u32 sCnt7[16];

static const char* load(void* block, void (*progress)(u32 done, u32 total), int* broken)
{
    *broken = 0;
    const char* problem = stateCheck(block);
    if (problem)
        return problem;
    const state_header_t* h = &sHeader;

    // Main ram, around: the stash window (through the stash file below), the
    // block (the live handshake) and the arm7 patch space if it is in main
    // memory (its sd routines are running).
    u32 keep[KEEP_RANGES][2];
    mainRamKeep(block, keep);
    u32 mainSteps = 0;
    restoreMainRam(keep, 0, &mainSteps, 0);
    // arm7 3, shared wram 2, tcms 2, palette and oam 2, vram 9, main ram,
    // stash 2 per part, io 1
    const u32 total = 19 + mainSteps + 2 * (RTS_STASH_SIZE / SCRATCH_SIZE) + 1;
    u32 done = 0;
    *broken = 1; // from here on the game's memory changes
#define STEP(x) do { if (!(x)) return "read failed"; progress(++done, total); } while (0)

    // arm7 wram and the arm7's context, through the vram A copy (filled later)
    u32 arm7Buffer = (u32)__vram_copy;
    STEP(readState(STATE_ARM7_IO, arm7Buffer, 512));
    STEP(readState(STATE_ARM7_WRAM, arm7Buffer + ARM7_CTX_SIZE, 0x10000));
    {
        const u32* cnt = (const u32*)(arm7Buffer + ARM7_CTX_SOUNDCNT_X);
        volatile u8* s7 = findShadow7((volatile u8*)(arm7Buffer + ARM7_CTX_SIZE), 0x10000);
        sShadow7Main = 0;
        if (s7)
            soundPlace(s7, cnt);
        else
        {
            u32 start = sRts->censusStart;
            if (start >= 0x02000000 && start < 0x023F8000 && !(start & 31))
                sShadow7Main = (volatile u8*)start; // looked for once main ram is the state's
            memcpy(sCnt7, cnt, sizeof(sCnt7));
        }
    }
    STEP(command(RTS_MENU_STATE_ARM7_LOAD, 0, arm7Buffer, 0x10000 / 512) == RTS_MENU_STATE_DONE);

    // shared wram, whoever owns which half
    *(vu8*)0x04000247 = h->vramcnt[7];
    u32 mode = h->vramcnt[7] & 3;
    for (u32 half = 0; half < 2; half++)
    {
        int arm7 = mode == 3 || (mode == 1 && half == 0) || (mode == 2 && half == 1);
        u32 offset = STATE_SHARED_WRAM + half * 0x4000;
        u32 dst = 0x037F8000 + (mode == 0 || mode == 3 ? half * 0x4000 : 0);
        STEP(arm7 ? readState(offset, dst, 0x4000) : readStateToArm9(offset, (volatile void*)dst, 0x4000));
    }

    // the tcms: dtcm holds the irq frames the game resumes from
    STEP(readStateToArm9(STATE_ITCM, (volatile void*)0x01FF8000, 0x8000));
    u32 dtcm = cp15Read(1) & ~0xFFF;
    STEP(readStateToArm9(STATE_DTCM, (volatile void*)dtcm, 0x4000));

    u16 menuBackdrop = *(vu16*)0x05000400; // the bottom screen stays the menu's until it leaves
    STEP(readStateToArm9(STATE_PALETTE, (volatile void*)0x05000000, 0x800));
    *(vu16*)0x05000400 = menuBackdrop;
    STEP(readStateToArm9(STATE_OAM, (volatile void*)0x07000000, 0x800));

    // vram: A into the copy the menu puts back when it leaves, the rest in place
    u32 offset = STATE_VRAM;
    for (u32 i = 0; i < sizeof(sBanks) / sizeof(sBanks[0]); i++)
    {
        if (i == 0)
        {
            STEP(readState(offset, (u32)__vram_copy, sBanks[0].size));
        }
        else
        {
            vu8* cnt = (vu8*)(0x04000240 + sBanks[i].cnt);
            *cnt = 0x80;
            int ok = readStateToArm9(offset, (volatile void*)sBanks[i].lcdc, sBanks[i].size);
            *cnt = h->vramcnt[sBanks[i].cnt];
            STEP(ok);
        }
        offset += sBanks[i].size;
    }

    if (!restoreMainRam(keep, progress, &done, total))
        return "read failed";
    for (u32 part = 0; part < RTS_STASH_SIZE; part += SCRATCH_SIZE)
    {
        STEP(readState(STATE_MAIN_RAM + keep[0][0] + part, (u32)__scratch, SCRATCH_SIZE));
        STEP(writeStash(part, (u32)__scratch, SCRATCH_SIZE));
    }

    // version 4's extra sector
    sLoadGx.valid = 0;
    if (h->version >= 4)
    {
        STEP(readState(STATE_EXTRA, (u32)__scratch, 512));
        const state_extra_t* x = (const state_extra_t*)__scratch;
        if (x->magic == STATE_EXTRA_MAGIC)
        {
            memcpy(sLoadGx.proj, x->gxProj, sizeof(sLoadGx.proj));
            memcpy(sLoadGx.pos, x->gxPos, sizeof(sLoadGx.pos));
            memcpy(sLoadGx.vec, x->gxVec, sizeof(sLoadGx.vec));
            sLoadGx.mode = x->gxMode & 3;
            sLoadGx.stat = x->gxStat;
            sLoadGx.valid = x->gxValid == 1;
        }
    }
    else
        progress(++done, total);
    // registers: the screen ones go back as the menu leaves
    STEP(readState(STATE_ARM9_IO, (u32)__scratch, STATE_ARM7_IO - STATE_ARM9_IO));
    for (u32 i = 0; i < sizeof(sIoRestore) / sizeof(sIoRestore[0]); i++)
        for (u32 b = 0; b < sIoRestore[i][1]; b += 2)
            *(vu16*)(0x04000000 + sIoRestore[i][0] + b) = *(u16*)(__scratch + sIoRestore[i][0] + b);
    for (int ch = 0; ch < 4; ch++)
        for (int i = 0; i < 3; i++)
            sDma[ch][i] = *(u32*)(__scratch + 0xB0 + ch * 12 + i * 4);
    // timers: as the menu leaves; older states have no reloads
    for (int i = 0; i < 4; i++)
        sLoadTimer[i] = h->version >= 4 ? h->timer9[i] : *(u32*)(__scratch + 0x100 + i * 4);
    memcpy(sLoadTimerReload, h->timerReload9, sizeof(sLoadTimerReload));
    sLoadTimerMeasured = h->version >= 4 ? h->timerMeasured9 & 0xF : 0;
    sLoaded = 1;
    // the loaded session's fifo words replace the running one's
    sFifo9[0] = h->fifo9Count <= 16 ? h->fifo9Count : 0;
    memcpy(&sFifo9[1], h->fifo9, 16 * 4);
    sFifo7[0] = h->fifo7Count <= 16 ? h->fifo7Count : 0;
    memcpy(&sFifo7[1], h->fifo7, 16 * 4);
    // the ipc sync output and irq enable (never its send bit), the fifo settings
    *(vu16*)0x04000180 = *(u16*)(__scratch + 0x180) & 0x4F00;
    REG_IPCFIFOCNT = (*(u16*)(__scratch + 0x184) & FIFOCNT_WRITABLE) | 0x4000;
    gGameScreen.dispcntA = h->dispcntA;
    gGameScreen.dispcntB = h->dispcntB;
    gGameScreen.dispcapcnt = h->dispcapcnt & ~(1u << 31); // no capture started by this
    gGameScreen.masterBrightA = h->masterBrightA;
    gGameScreen.masterBrightB = h->masterBrightB;
    gGameScreen.powcnt1 = h->powcnt1;
    gGameScreen.backdropB = h->backdropB;
    memcpy(gGameScreen.vramcnt, h->vramcnt, 10);

    // the cpu: what the menu leaves with, and the other modes' registers
    for (int i = 0; i < 8; i++)
        gSavedRegs[2 + i] = h->r4_r11[i];
    gSavedRegs[10] = h->irqLr;
    gSavedRegs[11] = h->irqSp;
    u32 banked[1 + 3 + 3 + 3 + 2 + 8];
    banked[0] = h->irqSpsr;
    memcpy(&banked[1], h->svc, 3 * 4);
    memcpy(&banked[4], h->abt, 3 * 4);
    memcpy(&banked[7], h->und, 3 * 4);
    memcpy(&banked[10], h->sys, 2 * 4);
    memcpy(&banked[12], h->fiq, 8 * 4);
    writeBankedRegs(banked);
    if (sShadow7Main)
    {
        volatile u8* s7 = findShadow7(sShadow7Main, 0x8000);
        if (s7)
            soundPlace(s7, sCnt7);
    }

    while (done < total)
        progress(++done, total);
    *broken = 0;
    return 0;
#undef STEP
}

// Saving reads main memory on the arm9 too (the zero map), the arm7's top 128
// KB with it: open every region to privileged access meanwhile, as for loads.
const char* stateSave(void* block, int slot, void (*progress)(u32 done, u32 total))
{
    u32 permissions = cp15Read(6);
    asm volatile("mcr p15, 0, %0, c5, c0, 2" :: "r"(0x11111111) : "memory");
    const char* problem = save(block, slot, progress);
    asm volatile("mcr p15, 0, %0, c5, c0, 2" :: "r"(permissions) : "memory");
    return problem;
}

const char* stateLoad(void* block, void (*progress)(u32 done, u32 total), int* broken)
{
    // The game's protection unit can keep the arm9 out of parts of main ram:
    // SDK games give the arm7's top 128 KB (0x023E0000) no arm9 access, and the
    // block's sector is there. The menu runs privileged, so open every region
    // to privileged reads and writes while it loads, then put the game's back.
    u32 permissions = cp15Read(6);
    asm volatile("mcr p15, 0, %0, c5, c0, 2" :: "r"(0x11111111) : "memory");
    const char* problem = load(block, progress, broken);
    asm volatile("mcr p15, 0, %0, c5, c0, 2" :: "r"(permissions) : "memory");
    return problem;
}
