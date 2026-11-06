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
void cp15WritePermissions(u32 v);
// The game's main ram: 4 MB, or 16 MB in dsi mode (set on entry, from the options).
extern u32 gMainSize;
#define TWL_MODE (gMainSize != 0x400000)
// In dsi mode, where a slot of the new wram is (0 when nowhere a cpu reads it), and
// its place and size in the file. info: the mapping as twlTakeInfo found it.
struct twl_info_s;
u32 twlSlot(int slot, int* arm7, u32* offset, u32* size);
u32 dtcmOverMain(u32* end);
// Main ram the load leaves alone (offsets from 0x02000000, [start, end)): the
// stash window, the block, the arm7 patch space when it is in main memory, the
// census space.
#define KEEP_RANGES 4
void mainRamKeep(void* block, u32 keep[KEEP_RANGES][2]);
// Whether a load sets this saved dma control again (else it stops the channel).
int dmaRestored(u32 control);
