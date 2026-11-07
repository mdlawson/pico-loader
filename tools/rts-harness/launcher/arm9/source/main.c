#include <unistd.h>
// Minimal launcher for testing Pico Loader in melonDS: boots /game.nds through
// /_pico/picoLoader9.bin and picoLoader7.bin, as Pico Loader's README describes.
// The game path can be overridden by /launch.txt (one line).
//
// Autopilot (with a loader built RTS_AUTOPILOT=1): /_pico/autolist.txt lists runs,
// one a line, rom|saveFrame[|gapFrames[|full|basic]] (the mode over the game's mode
// file, for timing the shadow against no shadow). Each boot of this launcher records the
// result of the run before (from /_pico/rtslog.txt, into /_pico/autoresults.txt),
// then starts the next: /_pico/autotest.txt tells the loader the save frame, and
// the game comes back here by itself once the menu has compared. Where it is goes
// in /_pico/autostate.txt; a run that never came back (a crash, then a power
// cycle) is recorded as such and the list goes on. Delete autostate.txt to start
// the list again.
#include <nds.h>
#include <fat.h>
#include <nds/arm9/dldi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "picoLoader7.h"

#define BOOT_MAGIC_ADDR ((vu32*)0x02FFFE70)
#define BOOT_ENTRY_ADDR ((vu32*)0x02FFFE74)
#define BOOT_MAGIC 0x504C4F44

static u32 sBuffer[0x20000 / 4]; // up to 128 KB per file

#define AUTO_LIST       "/_pico/autolist.txt"
#define AUTO_STATE      "/_pico/autostate.txt"
#define AUTO_RESULTS    "/_pico/autoresults.txt"
#define AUTO_TEST       "/_pico/autotest.txt"
#define RTS_LOG         "/_pico/rtslog.txt"

// line n (from 0) of the list, without the line end; 0 when there is none
static int autoEntry(int n, char* line, int size)
{
    FILE* f = fopen(AUTO_LIST, "r");
    if (!f)
        return 0;
    int i = 0, found = 0;
    while (fgets(line, size, f))
    {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#')
            continue;
        if (i++ == n)
        {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

// the number after the last line that starts with prefix, -1 without one
static long logNumber(const char* text, const char* prefix)
{
    long v = -1;
    size_t n = strlen(prefix);
    for (const char* p = text; p && *p; )
    {
        if (strncmp(p, prefix, n) == 0)
            v = strtol(p + n, NULL, 10);
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return v;
}

// the run before: its result line; then the next run: 1 and romPath when there is one
static int autopilotNext(char* romPath, int size)
{
    FILE* f = fopen(AUTO_LIST, "r");
    if (!f)
        return 0;
    fclose(f);
    int next = 0, running = -1;
    f = fopen(AUTO_STATE, "r");
    if (f)
    {
        if (fscanf(f, "next=%d running=%d", &next, &running) < 1)
            next = 0, running = -1;
        fclose(f);
    }
    char line[300];
    if (running >= 0 && autoEntry(running, line, sizeof(line)))
    {
        // the log as the menu left it (the launcher blanks it before each run)
        static char text[0x5001];
        size_t n = 0;
        FILE* log = fopen(RTS_LOG, "rb");
        if (log)
        {
            n = fread(text, 1, sizeof(text) - 1, log);
            fclose(log);
        }
        text[n] = 0;
        long memory = logNumber(text, "memory parts that differ: ");
        long registers = logNumber(text, "registers that differ: ");
        long run = logNumber(text, "parts that differ: ");
        long idle = logNumber(text, "idle per mille: ");
        long starved = logNumber(text, "starved frames: ");
        long timed = logNumber(text, "timed frames: ");
        char timing[64] = "";
        // no idle at all: the game waits some other way (the bios's IntrWait, a busy
        // loop) than the halt the loader times
        if (idle == 0 && timed > 0)
            snprintf(timing, sizeof(timing), "|idle n/a (no halts timed)");
        else if (idle >= 0)
            snprintf(timing, sizeof(timing), "|idle %ld.%ld%%|starved %ld/%ld", idle / 10, idle % 10, starved, timed);
        FILE* out = fopen(AUTO_RESULTS, "a");
        if (out)
        {
            if (run < 0)
                fprintf(out, "%d|%s|no result (crashed, hung or no menu)%s\n", running, line, timing);
            else
                fprintf(out, "%d|%s|memory %ld|registers %ld|run %ld%s\n", running, line, memory, registers, run, timing);
            fclose(out);
        }
        printf("run %d: %s\n", running, run < 0 ? "no result" : "done");
        next = running + 1;
    }
    if (!autoEntry(next, line, sizeof(line)))
    {
        f = fopen(AUTO_STATE, "w");
        if (f)
        {
            fprintf(f, "next=%d running=-1\n", next);
            fclose(f);
        }
        printf("autopilot: all %d runs done\nsee %s\n", next, AUTO_RESULTS);
        while (1) swiWaitForVBlank();
    }
    // rom|frame|gap
    char* frame = strchr(line, '|');
    if (!frame)
        return 0;
    *frame++ = 0;
    char* gap = strchr(frame, '|');
    if (gap)
        *gap++ = 0;
    char* mode = gap ? strchr(gap, '|') : NULL;
    if (mode)
        *mode++ = 0;
    // blank the log, its size and place kept (the loader made it contiguous)
    FILE* log = fopen(RTS_LOG, "r+b");
    if (log)
    {
        fseek(log, 0, SEEK_END);
        long size = ftell(log);
        fseek(log, 0, SEEK_SET);
        memset(sBuffer, ' ', sizeof(sBuffer));
        for (long done = 0; done < size; )
        {
            long chunk = size - done < (long)sizeof(sBuffer) ? size - done : (long)sizeof(sBuffer);
            fwrite(sBuffer, 1, chunk, log);
            done += chunk;
        }
        fclose(log);
    }
    f = fopen(AUTO_TEST, "w");
    if (f)
    {
        fprintf(f, "frame=%s\ngap=%s\n", frame, gap && gap[0] ? gap : "300");
        if (mode && mode[0])
            fprintf(f, "mode=%s\n", mode);
        fclose(f);
    }
    f = fopen(AUTO_STATE, "w");
    if (f)
    {
        fprintf(f, "next=%d running=%d\n", next, next);
        fclose(f);
    }
    strncpy(romPath, line, size - 1);
    romPath[size - 1] = 0;
    printf("autopilot run %d: %s at frame %s\n", next, romPath, frame);
    return 1;
}

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
    autopilotNext(romPath, sizeof(romPath));
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
