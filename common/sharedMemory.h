#pragma once
#include <nds/ndstypes.h>
#include <stddef.h>

#define BOOT_TYPE_INVALID   0
#define BOOT_TYPE_CARD      1
#define BOOT_TYPE_MULTIBOOT 2
#define BOOT_TYPE_NAND      3
#define BOOT_TYPE_MEMORY    4

struct shared_memory_multi_boot_info_t
{
    u16 bootType;
    u8 multibootInfo[0x40 - 2];
};

//0x02FFF800: 2KB
struct shared_memory_ntr_t
{
    u8 gap0[0x200];
    u8 gap200[0x80];
    u8 cardRomHeader[0x160]; // without debug footer
    u8 downloadParams[0x20];
    u8 bootCheckInfo[0x20];
    u32 resetParam;
    u8 gap424[8];
    u32 romOffset;
    u8 slot2Info[0xC];
    u32 vblankCounter;
    shared_memory_multi_boot_info_t multibootInfo;
    u8 firmwareUserData[0x100];
    u8 arm9ExceptionStack[0x1C];
    u32 arm9ExceptionVector;
    u8 arenaInfo[0x48];
    u8 rtcData[8];
    u8 systemConfig[6];
    u8 arm9Print;
    u8 arm7Print;
    u8 arm9ErrorPrint;
    u8 arm7ErrorPrint;
    u8 gap1FA[6];
    u8 romHeader[0x160];
    u8 isDebuggerData[0x20];
    u32 arm9IpcSignalParam;
    u32 arm7IpcSignalParam;
    u32 arm9IpcHandleChecker;
    u32 arm7IpcHandleChecker;
    u32 lastMicrophoneAddress;
    u16 microphoneSampleData;
    u16 wifiCallbackControl;
    u16 wifiRssiPool;
    u8 setSlot2InfoOnce;
    u8 slot2Inserted;
    u32 arm7Param;
    u32 arm9ThreadInfo;
    u32 arm7ThreadInfo;
    u16 padXYBuffer;
    u8 touchData[4];
    u16 autoloadSync;
    u8 arm9LockId[8];
    u8 arm7LockId[8];
    u8 vramCLock[8];
    u8 vramDLock[8];
    u8 wram0Lock[8];
    u8 wram1Lock[8];
    u8 slot1Lock[8];
    u8 slot2Lock[8];
    u8 initLock[8];
    u16 arm9MainMemoryCheck;
    u16 arm7MainMemoryCheck;
    u16 mainMemoryCmd;
};

static_assert(sizeof(shared_memory_ntr_t) == 0x800);

#define NTR_SHARED_MEMORY      ((shared_memory_ntr_t*)0x027FF800)
#define NTR_SHARED_MEMORY_SDK5 ((shared_memory_ntr_t*)0x02FFF800)

#define EXIT_REQUEST_REQUEST_MAGIC  0x54495845 // 'EXIT'
#define EXIT_REQUEST_PATH_MAGIC     0x58454C50 // 'PLEX'
#define EXIT_REQUEST_COMBO_MAGIC    0x424D4F43 // 'COMB'
#define EXIT_REQUEST_PARK_MAGIC     0x4B524150 // 'PARK'
#define EXIT_REQUEST_CANCEL_MAGIC   (EXIT_REQUEST_PARK_MAGIC + 1)
// RTS step 1 test (freeze for a second and resume). The arm9 check piece makes the
// request from the published combo magic: request = combo ^ EXIT_REQUEST_REQUEST_KEY.
// The arm7 answers a freeze request by holding for a second, then with CANCEL.
#define EXIT_REQUEST_REQUEST_KEY            (EXIT_REQUEST_COMBO_MAGIC ^ EXIT_REQUEST_REQUEST_MAGIC)
#define EXIT_REQUEST_FREEZE_COMBO_MAGIC     0x435A5246 // 'FRZC'
#define EXIT_REQUEST_FREEZE_REQUEST_MAGIC   (EXIT_REQUEST_FREEZE_COMBO_MAGIC ^ EXIT_REQUEST_REQUEST_KEY)
static_assert(EXIT_REQUEST_REQUEST_KEY == 0x16041706);
static_assert(EXIT_REQUEST_FREEZE_REQUEST_MAGIC == 0x555E4540);
// ExitComboPatchCode.s clears the arm9 ipc sync only for an exit, which it tells by bit 0
static_assert((EXIT_REQUEST_REQUEST_MAGIC & 1) && !(EXIT_REQUEST_FREEZE_REQUEST_MAGIC & 1));
#define EXIT_REQUEST_CHECKSUM_XOR   0x5A5A5A5A

// RTS step 2: the arm7 stashes RTS_STASH_SIZE bytes of main memory at RTS_BASE to
// RTS_STASH_PATH, loads RTS_MENU_PATH there and answers RUN, upon which the arm9
// wait piece jumps to RTS_BASE. The menu tells the arm7 its choice through the
// shared block: rts_info_t::menuState goes RTS_MENU_STATE_RESUME or _EXIT, the
// arm7 acknowledges by moving comboMagic off RUN, and the menu's very last act is
// RTS_MENU_STATE_LEFT, after which the arm7 may put the stash back.
// Size of the arm7 patch heap. Arm7Patcher (arm9) reserves it and NdsLoader (arm7)
// copies it into place when it lands in arm7 wram, so both must use this one value.
// The exit combo with the rts takes about 0x870 of it, the save code the rest.
#define ARM7_PATCH_SPACE_SIZE       0x1000

#define EXIT_REQUEST_RUN_MAGIC      (EXIT_REQUEST_PARK_MAGIC + 2)
#define RTS_INFO_MAGIC              0x31535452 // 'RTS1'
#define RTS_BASE                    0x02100000
#define RTS_STASH_SIZE              0x30000 // menu, its stack, and a copy of vram bank A
#define RTS_MENU_MAX_SIZE           0x8000
#define RTS_STASH_PATH              "/_pico/rtsstash.bin"
#define RTS_MENU_PATH               "/_pico/rtsmenu9.bin"
#define RTS_STATUS_PATH             "/_pico/rtsstatus.txt"
#define RTS_MENU_STATE_RESUME       0x4D534552 // 'RESM'
#define RTS_MENU_STATE_EXIT         0x54495845 // 'EXIT'
#define RTS_MENU_STATE_LEFT         0x5446454C // 'LEFT'

// While the menu runs it can have the arm7 move sectors between the sd card and
// memory: it fills rts_info_t::cmd*, then sets menuState to one of these; the
// arm7 answers RTS_MENU_STATE_DONE. Addresses are as the arm7 sees them.
#define RTS_MENU_STATE_SDREAD       0x44524453 // 'SDRD'
#define RTS_MENU_STATE_SDWRITE      0x52574453 // 'SDWR'
#define RTS_MENU_STATE_DONE         0x454E4F44 // 'DONE'
// For save states (layout in rtsmenu/state.h): ARM7_SAVE has the arm7 write its
// context (registers of every mode, irq state, sound and timer controls) to
// cmdAddress. ARM7_LOAD takes a context back from cmdAddress; with a nonzero
// cmdSectorCount it first copies the 64 KB after it over arm7 wram, all but
// the arm7 patch space. The arm7 answers DONE, or FAIL when the context is not
// from this same setup.
#define RTS_MENU_STATE_ARM7_SAVE    0x58433741 // 'A7CX'
#define RTS_MENU_STATE_ARM7_LOAD    0x444C3741 // 'A7LD'
#define RTS_MENU_STATE_FIFO_QUERY   0x51463741 // 'A7FQ': the arm7's kept fifo words to cmdAddress
#define RTS_MENU_STATE_FIFO_SEND    0x53463741 // 'A7FS': cmdSectorCount words to the arm9's fifo
#define RTS_MENU_STATE_ARM7_COPY    0x50433741 // 'A7CP': cmdSectorCount bytes from cmdSector (an address) to cmdAddress
#define RTS_MENU_STATE_FAIL         0x4C494146 // 'FAIL'

// Save states: one contiguous file per game and slot, made by the loader at boot.
#define RTS_STATE_DIRECTORY         "/_pico/states"
#define RTS_STATE_SIZE              0x500000 // layout in rtsmenu/state.h
#define RTS_LOG_PATH                "/_pico/rtslog.txt" // the restore test's report, text
#define RTS_LOG_SIZE                0x5000 // five 4 KB sections, the fifth the census

// The arm9 register-write census (arm9/source/patches/arm9/CensusPatchCode.s):
// variables and code, then the arrays from CENSUS_ARRAYS. Arm7Patcher takes it
// from the arm7's main memory arena, below the arm9's no-access region.
// The space is CENSUS_SPACE_SIZE when that leaves the arm7 CENSUS_ARM7_SPARE of
// its arena, else CENSUS_SPACE_MIN; the stub pool gets what the rest leaves.
#define CENSUS_SPACE_SIZE           0x3C00
#define CENSUS_SPACE_MIN            0x1C00
#define CENSUS_ARM7_SPARE           0x4000
#define CENSUS_ARRAYS               0x1000
#define CENSUS_POOL                 0x500 // from CENSUS_ARRAYS, 24 bytes a stub
#define CENSUS_MAGIC                0x31534E43 // 'CNS1'

/// @brief Where the arm7 finds the stash file and the menu on the sd card.
///        Both are single contiguous runs of sectors, checked by the loader at boot.
struct rts_info_t
{
    u32 magic;          // RTS_INFO_MAGIC when the rest is valid
    u32 base;           // RTS_BASE; the arm9 wait piece jumps here (block + 0x114)
    u32 stashSector;    // the stash file holds RTS_STASH_SIZE, then the menu
    u32 stashSectorCount;
    u32 menuSector;
    u32 menuSectorCount;
    u32 menuState;      // 0 while the menu runs, then RTS_MENU_STATE_* (block + 0x128)
    u32 stateSector;    // slot 1 save state file, 0 when there is none
    u32 stateSectorCount;
    u32 cmdSector;      // block + 0x134
    u32 cmdAddress;     // block + 0x138
    u32 cmdSectorCount; // block + 0x13C
    u32 arm7PatchStart; // the arm7 patch space, in arm7 wram or main memory (block + 0x140)
    u32 arm7PatchSize;  // block + 0x144
    u32 testFrames;     // block + 0x148: the arm7 counts it down on vblanks and freezes at 0
    u32 testMode;       // the menu's own: what the test freeze is for
    u32 testSlotSector[2]; // slots 2 and 3, the test's snapshots; 0 when missing
    u32 logSector;      // RTS_LOG_PATH, 0 when missing
    u32 logSectorCount;
    u32 testResult;     // the menu's own: the load check's counts
    u32 censusStart;    // the census space, 0 when there is none
};

static_assert(sizeof(rts_info_t) == 0x58);
// the arm7 stub stores the menu's exit answer as the exit request as it is
static_assert(RTS_MENU_STATE_EXIT == EXIT_REQUEST_REQUEST_MAGIC);

/// @brief Block used to request an in-game exit to the launcher.
///        The loader fills pathMagic, pathChecksum and launcherPath at boot;
///        the arm9 exit combo patch code only ever writes requestMagic, and the
///        arm7 exit patch code only ever writes comboMagic.
/// @note  The first four words deliberately share one cache line: the arm9 patch
///        code invalidates that single line to read the comboMagic published by
///        the arm7, which has no cache of its own.
struct exit_request_block_t
{
    u32 requestMagic;
    u32 pathMagic;
    u32 pathChecksum;

    /// @brief The arm7 side of the exit. COMB once the user has held the combo,
    ///        then PARK once the arm7 has stopped for the reboot, or CANCEL when
    ///        it could not stop safely and the game goes on. The arm7 owns the
    ///        whole detection because its patch heap has room to spare, while the
    ///        arm9 one is measured in a couple of hundred bytes. It also makes a
    ///        missing arm9 patch harmless: nothing ever consumes this and the
    ///        combo just does nothing.
    u32 comboMagic;

    char launcherPath[256];

    /// @brief RTS step 2, filled by the loader at boot next to the launcher path.
    rts_info_t rts;
};

static_assert(sizeof(exit_request_block_t) == 0x168);
static_assert(offsetof(exit_request_block_t, rts.testFrames) == 0x148);
static_assert(offsetof(exit_request_block_t, rts.cmdSector) == 0x134);
static_assert(offsetof(exit_request_block_t, rts.cmdSectorCount) == 0x13C);
static_assert(offsetof(exit_request_block_t, rts) == 0x110);
static_assert(offsetof(exit_request_block_t, rts.base) == 0x114);
static_assert(offsetof(exit_request_block_t, rts.menuState) == 0x128);

static inline u32 exit_request_calcChecksum(const char (&launcherPath)[256])
{
    u32 sum = 0;
    for (u32 i = 0; i < sizeof(launcherPath); i++)
        sum += (u8)launcherPath[i];
    return sum ^ EXIT_REQUEST_CHECKSUM_XOR;
}

//0x02FFC000: 16KB
struct shared_memory_twl_t
{
    u8 twlCardRomHeader[0x1000];
    u8 gap1000[0x7B0];
    char sysMenuVersionInfoContentId[9];
    char sysMenuVersionInfoContentLastInitialCode;
    u16 sysMenuDisableSetHotboot;
    u8 sdNandContext[0x44];
    u8 titleIdList[0x400];
    u8 mountInfo[0x3C0];
    u8 bootSrlPath[0x40];
    u8 twlRomHeader[0x1000];
    u8 sharedArena[0x680];
    exit_request_block_t exitRequest;
    u8 gap3790[0x180 - sizeof(exit_request_block_t)];
    shared_memory_ntr_t ntrSharedMem;
};

static_assert(sizeof(shared_memory_twl_t) == 0x4000);

#define TWL_SHARED_MEMORY ((shared_memory_twl_t*)0x02FFC000)
