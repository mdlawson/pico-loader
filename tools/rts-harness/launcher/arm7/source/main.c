// The launcher's arm7: runs the usual libnds services until the arm9 asks it
// to jump into Pico Loader's arm7 entry.
#include <nds.h>

#define BOOT_MAGIC_ADDR ((vu32*)0x02FFFE70)
#define BOOT_ENTRY_ADDR ((vu32*)0x02FFFE74)
#define BOOT_MAGIC 0x504C4F44

static void vblank(void)
{
}

int main(void)
{
    *BOOT_MAGIC_ADDR = 0;
    enableSound();
    readUserSettings();
    ledBlink(0);
    irqInit();
    fifoInit();
    installSystemFIFO();
    irqSet(IRQ_VBLANK, vblank);
    irqEnable(IRQ_VBLANK);
    while (*BOOT_MAGIC_ADDR != BOOT_MAGIC)
        swiWaitForVBlank();
    REG_IME = 0;
    REG_IE = 0;
    REG_IF = ~0;
    REG_SOUNDCNT = 0;
    for (int i = 0; i < 16; i++)
        SCHANNEL_CR(i) = 0;
    for (int i = 0; i < 4; i++)
        DMA_CR(i) = 0;
    u32 entry = *BOOT_ENTRY_ADDR;
    *BOOT_MAGIC_ADDR = 0;
    ((void (*)(void))entry)();
    while (1);
}
