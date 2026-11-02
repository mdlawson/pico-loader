// The real-time save menu's screen and input, run by the arm9 with irqs off
// from start.s. Draws on the top screen through vram bank A as a 256x192 direct
// colour framebuffer, as the DSTWO's in-game menu does, keeping the game's
// contents of the bank and every register it changes.

#include "rtsmenu.h"

#define REG_DISPCNT_A       (*(vu32*)0x04000000)
#define REG_DISPCNT_B       (*(vu32*)0x04001000)
#define REG_VCOUNT          (*(vu16*)0x04000006)
#define REG_DISPCAPCNT      (*(vu32*)0x04000064)
#define REG_MASTER_BRIGHT_A (*(vu32*)0x0400006C)
#define REG_MASTER_BRIGHT_B (*(vu32*)0x0400106C)
#define REG_KEYINPUT        (*(vu16*)0x04000130)
#define REG_VRAMCNT         ((vu8*)0x04000240)
#define REG_POWCNT1         (*(vu16*)0x04000304)
#define PALETTE_B           ((vu16*)0x05000400)

#define KEY_A       (1 << 0)
#define KEY_B       (1 << 1)
#define KEY_UP      (1 << 6)
#define KEY_DOWN    (1 << 7)

#define SCREEN_W    256
#define SCREEN_H    192
#define VRAM_A_SIZE 0x20000

#define RGB(r, g, b) ((u16)((r) | ((g) << 5) | ((b) << 10)))

extern const u16 gFont[]; // start.s: 8x16 cells, 2 bits per pixel, from ' '

game_screen_t gGameScreen;

static vu16* const sFrame = (vu16*)0x06800000; // bank A in lcdc mode

// gcc may turn struct and array initialisers into these
void* memcpy(void* dst, const void* src, unsigned int n)
{
    u8* d = dst;
    const u8* s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void* memset(void* dst, int value, unsigned int n)
{
    u8* d = dst;
    while (n--)
        *d++ = (u8)value;
    return dst;
}

static void copyWords(volatile u32* dst, const volatile u32* src, u32 count)
{
    while (count--)
        *dst++ = *src++;
}

static void waitVBlank(void)
{
    while (REG_VCOUNT == SCREEN_H);
    while (REG_VCOUNT != SCREEN_H);
}

static void takeScreens(void)
{
    // a display capture in flight finishes in the frame it started
    if (REG_DISPCAPCNT & (1u << 31))
        waitVBlank();
    gGameScreen.dispcntA = REG_DISPCNT_A;
    gGameScreen.dispcntB = REG_DISPCNT_B;
    gGameScreen.dispcapcnt = REG_DISPCAPCNT;
    gGameScreen.masterBrightA = REG_MASTER_BRIGHT_A;
    gGameScreen.masterBrightB = REG_MASTER_BRIGHT_B;
    gGameScreen.powcnt1 = REG_POWCNT1;
    gGameScreen.backdropB = PALETTE_B[0];
    for (int i = 0; i < 10; i++)
        gGameScreen.vramcnt[i] = REG_VRAMCNT[i];

    REG_DISPCAPCNT = 0;
    REG_VRAMCNT[0] = 0x80; // A enabled, lcdc
    copyWords(__vram_copy, (vu32*)sFrame, VRAM_A_SIZE / 4);

    // engine A shows bank A as is on the top screen, engine B only its backdrop
    REG_POWCNT1 = gGameScreen.powcnt1 | 0x8203; // lcds, 2d A and B, A on top; 3d as it was
    REG_MASTER_BRIGHT_A = 0;
    REG_MASTER_BRIGHT_B = 0;
    REG_DISPCNT_A = 2 << 16; // vram display, bank A
    REG_DISPCNT_B = 1 << 16; // normal display, every layer off
    PALETTE_B[0] = RGB(3, 4, 8);
}

static void giveScreensBack(void)
{
    waitVBlank();
    REG_DISPCNT_A = 0; // nothing shows bank A while it is filled
    copyWords((vu32*)sFrame, __vram_copy, VRAM_A_SIZE / 4);
    REG_VRAMCNT[0] = gGameScreen.vramcnt[0];
    PALETTE_B[0] = gGameScreen.backdropB;
    REG_MASTER_BRIGHT_A = gGameScreen.masterBrightA;
    REG_MASTER_BRIGHT_B = gGameScreen.masterBrightB;
    REG_POWCNT1 = gGameScreen.powcnt1;
    REG_DISPCNT_B = gGameScreen.dispcntB;
    REG_DISPCNT_A = gGameScreen.dispcntA;
    REG_DISPCAPCNT = gGameScreen.dispcapcnt;
    stateLeave();
}

// ---------------------------------------------------------------- drawing

static void fillRect(int x, int y, int w, int h, u16 color)
{
    for (int j = 0; j < h; j++)
    {
        vu16* p = sFrame + (y + j) * SCREEN_W + x;
        for (int i = 0; i < w; i++)
            p[i] = color;
    }
}

static u16 blend(u16 a, u16 b, int weight) // weight 0..3 of b
{
    u16 out = 0;
    for (int shift = 0; shift < 15; shift += 5)
    {
        int ca = (a >> shift) & 31, cb = (b >> shift) & 31;
        out |= ((ca * (3 - weight) + cb * weight) / 3) << shift;
    }
    return out;
}

static void drawText(int x, int y, const char* text, u16 ink, u16 paper)
{
    u16 shades[4] = { paper, blend(paper, ink, 1), blend(paper, ink, 2), ink };
    for (; *text; text++, x += 8)
    {
        u8 c = (u8)*text;
        if (c < 0x20 || c > 0x7E)
            c = '?';
        const u16* glyph = gFont + (c - 0x20) * 16;
        for (int row = 0; row < 16; row++)
        {
            vu16* p = sFrame + (y + row) * SCREEN_W + x;
            u16 bits = glyph[row];
            for (int col = 0; col < 8; col++, bits >>= 2)
                p[col] = shades[bits & 3];
        }
    }
}

// ---------------------------------------------------------------- menu

#define COLOR_BG        RGB(3, 4, 8)
#define COLOR_TITLE_BG  RGB(5, 9, 18)
#define COLOR_TEXT      RGB(27, 28, 30)
#define COLOR_DIM       RGB(15, 17, 21)
#define COLOR_SELECT_BG RGB(26, 27, 30)
#define COLOR_SELECT    RGB(3, 4, 8)
#define COLOR_GOOD      RGB(12, 26, 14)
#define COLOR_BAD       RGB(30, 12, 10)
#define COLOR_BAR       RGB(10, 18, 30)

enum { ITEM_RESUME, ITEM_SAVE, ITEM_LOAD, ITEM_EXIT };

static const char* const sLabels[] =
{
    [ITEM_RESUME] = "Resume game",
    [ITEM_SAVE] = "Save state (slot 1)",
    [ITEM_LOAD] = "Load state (slot 1)",
    [ITEM_EXIT] = "Exit to launcher",
};

static int sItems[4];
static int sItemCount;

#define ITEMS_Y     54
#define STATUS_Y    136

static void drawGameLine(const u8* header)
{
    char line[33] = "Game: ";
    int n = 6;
    for (int i = 0; i < 12 && header[i]; i++)
        line[n++] = header[i];
    line[n++] = ' ';
    line[n++] = '(';
    for (int i = 0; i < 4; i++)
        line[n++] = header[12 + i] ? header[12 + i] : '?';
    line[n++] = ')';
    line[n] = 0;
    drawText(8, 30, line, COLOR_DIM, COLOR_BG);
}

static void drawItems(int selected)
{
    for (int i = 0; i < sItemCount; i++)
    {
        int y = ITEMS_Y + i * 20;
        u16 paper = i == selected ? COLOR_SELECT_BG : COLOR_BG;
        u16 ink = i == selected ? COLOR_SELECT : COLOR_TEXT;
        fillRect(8, y - 2, SCREEN_W - 16, 20, paper);
        drawText(16, y, sLabels[sItems[i]], ink, paper);
    }
}

static void drawStatus(const char* text, u16 ink)
{
    fillRect(0, STATUS_Y, SCREEN_W, 20, COLOR_BG);
    if (text)
        drawText(8, STATUS_Y, text, ink, COLOR_BG);
}

static void drawProgress(u32 done, u32 total)
{
    // the step as text too, so a stop can be told apart from a slow step
    char count[8];
    int n = 0;
    if (done >= 10)
        count[n++] = '0' + done / 10 % 10;
    count[n++] = '0' + done % 10;
    count[n++] = '/';
    if (total >= 10)
        count[n++] = '0' + total / 10 % 10;
    count[n++] = '0' + total % 10;
    while (n < 7)
        count[n++] = ' ';
    count[n] = 0;
    drawText(SCREEN_W - 8 - 7 * 8, STATUS_Y, count, COLOR_DIM, COLOR_BG);
    const int x = 8, y = STATUS_Y + 18, w = SCREEN_W - 16, h = 6;
    int filled = (int)(done * (u32)w / total);
    fillRect(x, y, filled, h, COLOR_BAR);
    fillRect(x + filled, y, w - filled, h, COLOR_TITLE_BG);
}

static u16 keysDown(void)
{
    return ~REG_KEYINPUT & 0x3FF;
}

static void waitRelease(void)
{
    while (keysDown());
    // past the bounce
    for (vu32 i = 0; i < 0x20000; i++);
}

static void drawProblem(const char* prefix, const char* problem)
{
    char line[33];
    int n = 0;
    for (int i = 0; prefix[i] && n < 32; i++)
        line[n++] = prefix[i];
    for (int i = 0; problem[i] && n < 32; i++)
        line[n++] = problem[i];
    line[n] = 0;
    drawStatus(line, COLOR_BAD);
}

static void saveState(void* block)
{
    drawStatus("Saving state...", COLOR_TEXT);
    drawProgress(0, 1);
    const char* problem = stateSave(block, drawProgress);
    fillRect(0, STATUS_Y + 18, SCREEN_W, 6, COLOR_BG);
    if (problem)
    {
        drawProblem("Save failed: ", problem);
    }
    else
    {
        drawStatus("Saved to slot 1.", COLOR_GOOD);
    }
}

// Asks for A once more, then loads. Returns 1 when the game should resume
// from the loaded state; sets *broken when it can only exit.
static int loadState(void* block, int* broken)
{
    *broken = 0;
    const char* problem = stateCheck(block);
    if (problem)
    {
        drawProblem("Can't load: ", problem);
        return 0;
    }
    drawStatus("A: load slot 1, B: cancel", COLOR_TEXT);
    u16 keys;
    while (!((keys = keysDown()) & (KEY_A | KEY_B)));
    waitRelease();
    if (keys & KEY_B)
    {
        drawStatus(0, 0);
        return 0;
    }
    drawStatus("Loading state...", COLOR_TEXT);
    drawProgress(0, 1);
    problem = stateLoad(block, drawProgress, broken);
    fillRect(0, STATUS_Y + 18, SCREEN_W, 6, COLOR_BG);
    if (problem)
    {
        drawProblem(*broken ? "Load broke: " : "Can't load: ", problem);
        return 0;
    }
    return 1;
}

u32 rtsMain(void* block)
{
    volatile rts_info_t* rts = (volatile rts_info_t*)((u8*)block + RTS_INFO_OFFSET);
    stateTakeIo();
    takeScreens();
    fillRect(0, 0, SCREEN_W, SCREEN_H, COLOR_BG);
    fillRect(0, 0, SCREEN_W, 22, COLOR_TITLE_BG);
    drawText(8, 3, "Pico Loader RTS", COLOR_TEXT, COLOR_TITLE_BG);
    drawGameLine((const u8*)block + HEADER_FROM_BLOCK);
    drawText(8, SCREEN_H - 20, "A: choose  B: back to the game", COLOR_DIM, COLOR_BG);

    sItemCount = 0;
    sItems[sItemCount++] = ITEM_RESUME;
    if (rts->stateSector != 0)
    {
        sItems[sItemCount++] = ITEM_SAVE;
        sItems[sItemCount++] = ITEM_LOAD;
    }
    sItems[sItemCount++] = ITEM_EXIT;
    if (rts->stateSector == 0)
        drawStatus("No state file: see rtsstatus.txt", COLOR_DIM);

    int selected = 0;
    drawItems(selected);

    // the combo is still held
    waitRelease();
    u32 answer = 0;
    while (!answer)
    {
        u16 keys = keysDown();
        if (!keys)
            continue;
        if (keys & KEY_UP)
            selected = selected == 0 ? sItemCount - 1 : selected - 1;
        else if (keys & KEY_DOWN)
            selected = selected == sItemCount - 1 ? 0 : selected + 1;
        else if (keys & KEY_A)
        {
            switch (sItems[selected])
            {
                case ITEM_RESUME: answer = RTS_MENU_STATE_RESUME; break;
                case ITEM_EXIT: answer = RTS_MENU_STATE_EXIT; break;
                case ITEM_SAVE: waitRelease(); saveState(block); break;
                case ITEM_LOAD:
                {
                    waitRelease();
                    int broken;
                    if (loadState(block, &broken))
                        answer = RTS_MENU_STATE_RESUME;
                    else if (broken)
                    {
                        // the game's memory is half one state, half the other
                        sItemCount = 0;
                        sItems[sItemCount++] = ITEM_EXIT;
                        selected = 0;
                        fillRect(0, ITEMS_Y - 2, SCREEN_W, 4 * 20, COLOR_BG);
                    }
                    break;
                }
            }
        }
        else if ((keys & KEY_B) && sItems[0] == ITEM_RESUME)
            answer = RTS_MENU_STATE_RESUME;
        waitVBlank();
        drawItems(selected);
        waitRelease(); // one step per press, and the game should not see the key
    }

    giveScreensBack();
    return answer;
}
