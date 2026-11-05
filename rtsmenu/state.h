#pragma once
// Save state file layout, version 3 (version 2 files load too: their extra
// fields are 0). One file per game and slot, RTS_STATE_SIZE
// (common/sharedMemory.h) bytes, made contiguous by the loader. Offsets are bytes
// from the start of the file, all sector aligned. The header is written last:
// a state is valid only when its magic and version match.

#include <stdint.h>

#define STATE_MAGIC             0x53545250 // 'PRTS'
#define STATE_VERSION           5 // 2: with the arm7 context; 3: ipc fifo words, more arm7 registers;
                                  // 4: the arm9 timers' counts and measured reloads; 5: the zero map

#define STATE_HEADER            0x000000 // state_header_t, 0x200
#define STATE_ARM9_IO           0x000200 // 0x1100: 0x04000000-0x04000FFF, then 0x04001000-0x040010FF;
                                         // only the readable registers, the rest is 0
#define STATE_ARM9_IO_SIZE      0x1100
#define STATE_ARM7_IO           0x001400 // 0x400: the arm7 context, ARM7_CTX_* below
#define ARM7_CTX_SIZE           0x100
#define STATE_EXTRA             0x001800 // 0x200, version 4: state_extra_t
#define STATE_ZERO_MAP          0x001A00 // 0x200, version 5 with zeroMap: a bit per 2 KB block of the
                                         // file (bit b & 31 of word b >> 5), set when the block is all
                                         // zero and was not written; only from STATE_VRAM on
#define STATE_ZERO_MAP_SIZE     0x200
#define STATE_ZERO_BLOCK        0x800
#define STATE_ITCM              0x004000 // 0x8000, read at 0x01FF8000
#define STATE_DTCM              0x00C000 // 0x4000, read at the dtcm base
#define STATE_PALETTE           0x010000 // 0x800, 0x05000000
#define STATE_OAM               0x010800 // 0x800, 0x07000000
#define STATE_SHARED_WRAM       0x011000 // 0x8000, both halves in order whoever owns them
#define STATE_ARM7_WRAM         0x019000 // 0x10000, 0x03800000
#define STATE_VRAM              0x029000 // 0xA4000: banks A-I in order, each in full
#define STATE_MAIN_RAM          0x100000 // 0x400000, 0x02000000
#define STATE_END               0x500000

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t headerSize;
    uint32_t gameCode;
    char title[12];
    uint32_t blockAddress;      // exit_request_block_t in this game's mirror

    // the arm9 as the menu got it from the wait piece (irq mode, irqs off)
    uint32_t waitEntry;         // r0, where the menu goes back to
    uint32_t r4_r11[8];
    uint32_t irqLr;             // back into the hook
    uint32_t irqSp;
    uint32_t irqSpsr;
    uint32_t svc[3];            // sp, lr, spsr
    uint32_t abt[3];
    uint32_t und[3];
    uint32_t sys[2];            // sp, lr
    uint32_t fiq[8];            // r8-r12, sp, lr, spsr

    // cp15
    uint32_t control;
    uint32_t dtcmRegion;
    uint32_t itcmRegion;
    uint32_t dataCacheable;
    uint32_t insnCacheable;
    uint32_t writeBufferable;
    uint32_t dataPermissions;
    uint32_t insnPermissions;
    uint32_t regions[8];

    // the game's values of the registers the menu changes (the io dump has them too)
    uint8_t vramcnt[10];        // 0x04000240-0x04000249: A-G, WRAMCNT, H, I
    uint16_t powcnt1;
    uint32_t dispcntA;
    uint32_t dispcntB;
    uint32_t dispcapcnt;
    uint32_t masterBrightA;
    uint32_t masterBrightB;
    uint16_t backdropB;
    uint16_t reserved0;

    uint32_t stashBase;         // RTS_BASE; the main ram there comes from the stash file
    uint32_t stashSize;

    // ipc fifo words sent and not read yet, as the freeze found them
    uint32_t fifo9Count;        // in the arm9's receive fifo (sent by the arm7)
    uint32_t fifo9[16];
    uint32_t fifo7Count;        // in the arm7's receive fifo (sent by the arm9)
    uint32_t fifo7[16];

    // version 4: the arm9 timers as the menu found them (count | control << 16),
    // stopped while it ran; their reloads where it could measure them
    uint32_t timer9[4];
    uint16_t timerReload9[4];
    uint32_t timerMeasured9;    // bit per timer: timerReload9 is valid

    // version 5: 1 when STATE_ZERO_MAP is valid
    uint32_t zeroMap;

} state_header_t;

#define STATE_EXTRA_MAGIC       0x34585250 // 'PRX4'

// What else version 4 keeps, in its own sector.
typedef struct
{
    uint32_t magic;             // STATE_EXTRA_MAGIC when valid
    // the geometry engine's current matrices (20.12 fixed point), read
    // through the 3d result registers; valid when gxValid is 1
    int32_t gxProj[16];
    int32_t gxPos[16];
    int32_t gxVec[9];           // 3x3
    uint32_t gxMode;            // MTX_MODE as found
    uint32_t gxStat;            // GXSTAT as found: the stack levels
    uint32_t gxValid;
} state_extra_t;

_Static_assert(sizeof(state_header_t) <= 0x200, "state header is one sector");
_Static_assert(sizeof(state_extra_t) <= 0x200, "extra is one sector");
_Static_assert(STATE_ARM7_IO + 0x400 <= STATE_EXTRA, "extra after the arm7 context");
_Static_assert(STATE_ZERO_MAP + STATE_ZERO_MAP_SIZE <= STATE_ITCM && STATE_END / STATE_ZERO_BLOCK / 8 <= STATE_ZERO_MAP_SIZE, "zero map fits");
_Static_assert(STATE_VRAM % STATE_ZERO_BLOCK == 0, "the mapped part starts on a block");
_Static_assert(STATE_ARM9_IO + STATE_ARM9_IO_SIZE <= STATE_ARM7_IO, "io dump fits");
_Static_assert(STATE_VRAM + 0xA4000 <= STATE_MAIN_RAM, "vram fits");
_Static_assert(STATE_MAIN_RAM + 0x400000 == STATE_END, "main ram ends the file");

// The arm7 context, as ExitComboArm7PatchCode.s writes and reads it.
#define ARM7_CTX_MAGIC          0x00 // RTS_MENU_STATE_ARM7_SAVE
#define ARM7_CTX_SPSR_IRQ       0x04
#define ARM7_CTX_SP_IRQ         0x08 // at the stub's command loop
#define ARM7_CTX_SVC            0x0C // spsr, sp, lr
#define ARM7_CTX_ABT            0x18
#define ARM7_CTX_UND            0x24
#define ARM7_CTX_SYS            0x30 // sp, lr
#define ARM7_CTX_FIQ            0x38 // r8-r12, sp, lr, spsr
#define ARM7_CTX_IE             0x58
#define ARM7_CTX_IME            0x5C // as the game had them before the freeze
#define ARM7_CTX_SOUNDCNT       0x60
#define ARM7_CTX_IF             0x64
#define ARM7_CTX_POWCNT2        0x68
#define ARM7_CTX_SOUNDBIAS      0x6C
#define ARM7_CTX_TIMERS         0x70 // 4 words, TMxCNT
#define ARM7_CTX_SOUNDCNT_X     0x80 // 16 words, SOUNDxCNT
#define ARM7_CTX_WRAM_WORDS     0xC0 // 9 words of the game's under the stub's wait loop
#define ARM7_CTX_STUB           0xE4 // where the stub was: the same build
// version 3 (valid when ARM7_CTX_EXT_MAGIC holds 'RTX3'):
#define ARM7_CTX_DISPSTAT_IPCSYNC 0xE8
#define ARM7_CTX_FIFOCNT_SPICNT 0xEC
#define ARM7_CTX_SNDCAPCNT      0xF0
#define ARM7_CTX_DMACNT_H       0xF4 // two words: channels 0|1 << 16, 2|3 << 16
#define ARM7_CTX_EXT_MAGIC      0xFC
