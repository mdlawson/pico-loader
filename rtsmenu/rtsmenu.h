#pragma once
// Shared between the rts menu's files.

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;

// mirrors common/sharedMemory.h
#define RTS_BASE                0x02100000
#define RTS_STASH_SIZE          0x30000
#define RTS_STATE_SIZE          0x500000
#define RTS_MENU_STATE_RESUME   0x4D534552 // 'RESM'
#define RTS_MENU_STATE_EXIT     0x54495845 // 'EXIT'
#define RTS_MENU_STATE_SDREAD   0x44524453 // 'SDRD'
#define RTS_MENU_STATE_SDWRITE  0x52574453 // 'SDWR'
#define RTS_MENU_STATE_DONE     0x454E4F44 // 'DONE'
#define RTS_MENU_STATE_FAIL     0x4C494146 // 'FAIL'
#define RTS_MENU_STATE_ARM7_SAVE 0x58433741 // 'A7CX'
#define RTS_MENU_STATE_ARM7_LOAD 0x444C3741 // 'A7LD'

typedef struct
{
    u32 magic;
    u32 base;
    u32 stashSector;
    u32 stashSectorCount;
    u32 menuSector;
    u32 menuSectorCount;
    u32 menuState;
    u32 stateSector;
    u32 stateSectorCount;
    u32 cmdSector;
    u32 cmdAddress;
    u32 cmdSectorCount;
    u32 arm7PatchStart;
    u32 arm7PatchSize;
} rts_info_t;

#define RTS_INFO_OFFSET         0x110 // in exit_request_block_t
// the cartridge header, at the same distance from the block in both mirrors
// (0x027FF680 -> 0x027FFE00, 0x02FFF680 -> 0x02FFFE00)
#define HEADER_FROM_BLOCK       0x780

// the game's state of what the menu changes, taken before it changes anything
typedef struct
{
    u32 dispcntA;
    u32 dispcntB;
    u32 dispcapcnt;
    u32 masterBrightA;
    u32 masterBrightB;
    u16 powcnt1;
    u16 backdropB;
    u8 vramcnt[10]; // 0x04000240-0x04000249
} game_screen_t;

extern game_screen_t gGameScreen;
extern u32 gSavedRegs[12]; // start.s: r0, r1, r4-r11, lr, sp as the menu was entered
extern u32 __vram_copy[];  // rtsmenu.ld: the game's vram bank A
extern u8 __scratch[];     // rtsmenu.ld: 0x8000 bytes the arm7 can read and write
#define SCRATCH_SIZE 0x8000

void readBankedRegs(u32* out); // start.s: spsr_irq, svc, abt, und, sys, fiq
void writeBankedRegs(const u32* in); // start.s: the same, back

// state.c
void stateTakeIo(void);
// Writes the save state. progress(done, total) is called between steps.
// Returns 0 on success, or a short reason.
const char* stateSave(void* block, void (*progress)(u32 done, u32 total));
// Checks slot 1 holds a state this game can load here. 0 if so, or why not.
const char* stateCheck(void* block);
// Loads it. Before anything changes it checks again and returns why not; once
// it has started, *broken is set when it could not finish (the game's memory
// is then a mix, and only exiting makes sense).
const char* stateLoad(void* block, void (*progress)(u32 done, u32 total), int* broken);
// After a load, sets what has to wait until the menu is gone (the dma channels).
void stateLeave(void);

static inline void dcFlushAll(void)
{
    // clean and invalidate by set/way: 4 ways of 32 lines of 32 bytes
    for (u32 way = 0; way < 4; way++)
        for (u32 line = 0; line < 32; line++)
            asm volatile("mcr p15, 0, %0, c7, c14, 2" :: "r"((way << 30) | (line << 5)) : "memory");
    asm volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

static inline void dcInvalidateLine(const volatile void* p)
{
    asm volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(p) : "memory");
}
