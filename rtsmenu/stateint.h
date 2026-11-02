#pragma once
// Between state.c and check.c.

#include "rtsmenu.h"

extern volatile rts_info_t* sRts;

typedef struct
{
    u8 cnt;     // offset of its VRAMCNT from 0x04000240
    u32 lcdc;   // where lcdc mode puts it
    u32 size;
} vram_bank_t;
extern const vram_bank_t sBanks[9]; // A-I in state file order

// Returns the arm7's answer: RTS_MENU_STATE_DONE, or _FAIL.
u32 command(u32 cmd, u32 sector, u32 address, u32 sectorCount);
// From slot 1, 2 or 3; offset and size in whole sectors. 1 when done.
int readSlot(int slot, u32 offset, u32 address, u32 size);
u32 slotSector(int slot);
void copyWords(volatile u32* dst, const volatile u32* src, u32 bytes);
u32 cp15Read(int which);
// Main ram the load leaves alone (offsets from 0x02000000, [start, end)): the
// stash window, the block, the arm7 patch space when it is in main memory, the
// census space.
#define KEEP_RANGES 4
void mainRamKeep(void* block, u32 keep[KEEP_RANGES][2]);
// Whether a load sets this saved dma control again (else it stops the channel).
int dmaRestored(u32 control);
