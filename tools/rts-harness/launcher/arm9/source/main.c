#include <unistd.h>
// Minimal launcher for testing Pico Loader in melonDS: boots /game.nds through
// /_pico/picoLoader9.bin and picoLoader7.bin, as Pico Loader's README describes.
// The game path can be overridden by /launch.txt (one line).
#include <nds.h>
#include <fat.h>
#include <nds/arm9/dldi.h>
#include <stdio.h>
#include <string.h>
#include "picoLoader7.h"

#define BOOT_MAGIC_ADDR ((vu32*)0x02FFFE70)
#define BOOT_ENTRY_ADDR ((vu32*)0x02FFFE74)
#define BOOT_MAGIC 0x504C4F44

static u32 sBuffer[0x20000 / 4]; // up to 128 KB per file

static int loadToVram(const char* path, vu32* dst, u32 max)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return -1;
    int n = fread(sBuffer, 1, max, f);
    fclose(f);
    for (int i = 0; i < (n + 3) / 4; i++)
        dst[i] = sBuffer[i];
    return n;
}

int main(void)
{
    consoleDemoInit();
    printf("RTS test launcher\n");
    // On a DSi this fails without an sd card in the console's own slot, but the
    // flashcart's card (fat:, through DLDI) is mounted anyway; use that one.
    bool fatOk = fatInitDefault();
    if (chdir("fat:/") != 0 && !fatOk)
    {
        printf("no fat: and fatInitDefault failed\n");
        while (1) swiWaitForVBlank();
    }
    printf("cwd fat:/ (fatInitDefault %d)\n", fatOk);
    char romPath[256] = "/game.nds";
    FILE* t = fopen("/launch.txt", "r");
    if (t)
    {
        if (fgets(romPath, sizeof(romPath), t))
            romPath[strcspn(romPath, "\r\n")] = 0;
        fclose(t);
    }
    printf("rom: %s\n", romPath);

    vramSetBankA(VRAM_A_LCD);
    vramSetBankB(VRAM_B_LCD);
    vramSetBankC(VRAM_C_LCD);
    vramSetBankD(VRAM_D_LCD);
    int n9 = loadToVram("/_pico/picoLoader9.bin", (vu32*)0x06800000, 0x40000 > sizeof(sBuffer) ? sizeof(sBuffer) : 0x40000);
    int n7 = loadToVram("/_pico/picoLoader7.bin", (vu32*)0x06840000, sizeof(sBuffer));
    printf("loader9 %d loader7 %d\n", n9, n7);
    if (n9 <= 0 || n7 <= 0)
        while (1) swiWaitForVBlank();

    // the header: build it in ram, then copy it in with word writes (vram)
    static pload_header7_t header;
    memcpy(&header, (const void*)0x06840000, sizeof(header));
    // like Pico Launcher, which leaves the header's dldiDriver as the file has it,
    // when /nodldi.txt exists
    FILE* nd = fopen("/nodldi.txt", "rb");
    if (nd) fclose(nd);
    else header.dldiDriver = (void*)io_dldi_data;
    printf("dldiDriver %p\n", header.dldiDriver);
    header.bootDrive = PLOAD_BOOT_DRIVE_DLDI;
    memset(&header.loadParams, 0, sizeof(header.loadParams));
    strcpy(header.loadParams.romPath, romPath);
    strcpy(header.loadParams.savePath, romPath);
    char* dot = strrchr(header.loadParams.savePath, '.');
    if (dot) strcpy(dot, ".sav");
    if (header.apiVersion >= 2)
    {
        memset(header.v2.launcherPath, 0, sizeof(header.v2.launcherPath));
        strcpy(header.v2.launcherPath, "/launcher.nds");
    }
    if (header.apiVersion >= 3)
        header.v3.cheats = NULL;
    u32 entry7 = (u32)header.entryPoint;
    for (u32 i = 0; i < sizeof(header) / 4; i++)
        ((vu32*)0x06840000)[i] = ((u32*)&header)[i];
    printf("api %d entry7 %08lx\n", header.apiVersion, entry7);

    REG_IME = 0;
    REG_IE = 0;
    REG_IF = ~0;
    for (int i = 0; i < 4; i++)
        DMA_CR(i) = 0;
    DC_FlushAll();
    DC_InvalidateAll();
    IC_InvalidateAll();
    vramSetBankC(VRAM_C_ARM7_0x06000000);
    vramSetBankD(VRAM_D_ARM7_0x06020000);
    *BOOT_ENTRY_ADDR = entry7;
    *BOOT_MAGIC_ADDR = BOOT_MAGIC;
    DC_FlushAll();
    ((void (*)(void))0x06800000)();
    while (1);
}
