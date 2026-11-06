#pragma once
// Shared between the rts menu's files.

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;

// mirrors common/sharedMemory.h
#define RTS_BASE                0x02100000
#define RTS_STASH_SIZE          0x30000
#define RTS_STATE_SIZE          0x500000
#define CENSUS_SPACE_SIZE       0x3C00
#define CENSUS_ARRAYS_OFFSET    0x1000 // variables and code before, shadow, patch table and stubs after
#define RTS_MENU_STATE_RESUME   0x4D534552 // 'RESM'
#define RTS_MENU_STATE_EXIT     0x54495845 // 'EXIT'
#define RTS_MENU_STATE_SDREAD   0x44524453 // 'SDRD'
#define RTS_MENU_STATE_SDWRITE  0x52574453 // 'SDWR'
#define RTS_MENU_STATE_DONE     0x454E4F44 // 'DONE'
#define RTS_MENU_STATE_FAIL     0x4C494146 // 'FAIL'
#define RTS_MENU_STATE_ARM7_SAVE 0x58433741 // 'A7CX'
#define RTS_MENU_STATE_ARM7_LOAD 0x444C3741 // 'A7LD'
#define RTS_MENU_STATE_FIFO_QUERY 0x51463741 // 'A7FQ'
#define RTS_MENU_STATE_FIFO_SEND 0x53463741 // 'A7FS'
#define RTS_MENU_STATE_ARM7_COPY 0x50433741 // 'A7CP'

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
    u32 testFrames;     // the arm7 counts it down on vblanks and freezes at 0
    u32 testMode;       // TEST_*: what the next test freeze is for
    u32 testSlotSector[2]; // slots 2 and 3
    u32 logSector;
    u32 logSectorCount;
    u32 testResult;     // the load check's counts: memory parts | registers << 16
    u32 censusStart;    // the census space, 0 when there is none
    u32 options;        // RTS_OPTION_*
    u32 configSector;   // the game's mode file, one sector; 0 when missing
} rts_info_t;

#define RTS_CONFIG_SIZE 512
#define RTS_OPTION_BASIC        1 // this boot is basic: no register shadow
#define RTS_OPTION_NEXT_BASIC   2 // what the mode file says now

#define TEST_REFERENCE  1 // the freeze writes slot 2
#define TEST_COMPARE    2 // the freeze writes slot 3 and compares it with slot 2
#define TEST_FRAMES     120

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
extern u8 __scratch[];     // rtsmenu.ld: SCRATCH_SIZE bytes the arm7 can read and write
#define SCRATCH_SIZE 0x4000

void readBankedRegs(u32* out); // start.s: spsr_irq, svc, abt, und, sys, fiq
void writeBankedRegs(const u32* in); // start.s: the same, back

// state.c
// First thing on entry: takes the words waiting in the arm9's ipc fifo.
void stateEnter(void* block);
void stateTakeIo(void);
// Writes the save state. progress(done, total) is called between steps.
// Returns 0 on success, or a short reason.
// slot 1, or 2 and 3 for the test.
const char* stateSave(void* block, int slot, void (*progress)(u32 done, u32 total));
// Checks slot 1 holds a state this game can load here. 0 if so, or why not.
const char* stateCheck(void* block);
// Loads it. Before anything changes it checks again and returns why not; once
// it has started, *broken is set when it could not finish (the game's memory
// is then a mix, and only exiting makes sense).
const char* stateLoad(void* block, void (*progress)(u32 done, u32 total), int* broken);
// Last thing before resuming, the screens given back: both ipc fifos as they
// were (or as the loaded state had them), and after a load the dma channels.
void stateLeave(void);

// check.c: the restore test, reported in /_pico/rtslog.txt
// After stateLoad: the memory against slot 1. Returns the parts that differ.
u32 checkLoadedMemory(void* block, void (*progress)(u32 done, u32 total));
// At stateLeave after a load + compare: the registers against slot 1, then the
// log written. Returns the registers that differ.
u32 checkLoadedRegisters(void);
// Slot 2 against slot 3. Returns the parts that differ; *writtenLog 0 if the log failed.
u32 compareTestSlots(void* block, void (*progress)(u32 done, u32 total));
// Asks stateLeave to check the registers and write the log.
extern int gCheckOnLeave;

static inline void dcFlushAll(void)
{
    // clean and invalidate by set/way: 4 ways of 32 lines of 32 bytes
    for (u32 way = 0; way < 4; way++)
        for (u32 line = 0; line < 32; line++)
            asm volatile("mcr p15, 0, %0, c7, c14, 2" :: "r"((way << 30) | (line << 5)) : "memory");
    asm volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

static inline void icInvalidateAll(void)
{
    asm volatile("mcr p15, 0, %0, c7, c5, 0" :: "r"(0) : "memory");
}

static inline void dcInvalidateLine(const volatile void* p)
{
    asm volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(p) : "memory");
}

// The census report into the log's fifth section (check.c); 0 when not written.
int censusWriteLog(void* block, char* line1, char* line2);
