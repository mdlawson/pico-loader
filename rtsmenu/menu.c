// The real-time save menu's screen and input, run by the arm9 with irqs off
// from start.s. Draws on the top screen through vram bank A as a 256x192 direct
// colour framebuffer, as the DSTWO's in-game menu does, keeping the game's
// contents of the bank and every register it changes.

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;

// mirrors common/sharedMemory.h
#define RTS_MENU_STATE_RESUME   0x4D534552 // 'RESM'
#define RTS_MENU_STATE_EXIT     0x54495845 // 'EXIT'
// the cartridge header, at the same distance from the exit request block in
// both mirrors (0x027FF680 -> 0x027FFE00, 0x02FFF680 -> 0x02FFFE00)
#define HEADER_FROM_BLOCK       0x780

#define REG_DISPCNT_A       (*(vu32*)0x04000000)
#define REG_DISPCNT_B       (*(vu32*)0x04001000)
#define REG_VCOUNT          (*(vu16*)0x04000006)
#define REG_DISPCAPCNT      (*(vu32*)0x04000064)
#define REG_MASTER_BRIGHT_A (*(vu32*)0x0400006C)
#define REG_MASTER_BRIGHT_B (*(vu32*)0x0400106C)
#define REG_KEYINPUT        (*(vu16*)0x04000130)
#define REG_VRAMCNT_A       (*(vu8*)0x04000240)
#define REG_POWCNT1         (*(vu16*)0x04000304)
#define PALETTE_B           ((vu16*)0x05000400)

#define KEY_A       (1 << 0)
#define KEY_B       (1 << 1)
#define KEY_SELECT  (1 << 2)
#define KEY_START   (1 << 3)
#define KEY_UP      (1 << 6)
#define KEY_DOWN    (1 << 7)

#define SCREEN_W    256
#define SCREEN_H    192
#define VRAM_A_SIZE 0x20000

#define RGB(r, g, b) ((u16)((r) | ((g) << 5) | ((b) << 10)))

extern const u16 gFont[]; // start.s: 8x16 cells, 2 bits per pixel, from ' '
extern u32 __vram_copy[];  // rtsmenu.ld: room for vram bank A

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

static vu16* const sFrame = (vu16*)0x06800000; // bank A in lcdc mode

static struct
{
    u32 dispcntA;
    u32 dispcntB;
    u32 dispcapcnt;
    u32 masterBrightA;
    u32 masterBrightB;
    u16 powcnt1;
    u16 backdropB;
    u8 vramcntA;
} sSaved;

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
    sSaved.dispcntA = REG_DISPCNT_A;
    sSaved.dispcntB = REG_DISPCNT_B;
    sSaved.dispcapcnt = REG_DISPCAPCNT;
    sSaved.masterBrightA = REG_MASTER_BRIGHT_A;
    sSaved.masterBrightB = REG_MASTER_BRIGHT_B;
    sSaved.powcnt1 = REG_POWCNT1;
    sSaved.backdropB = PALETTE_B[0];
    sSaved.vramcntA = REG_VRAMCNT_A;

    REG_DISPCAPCNT = 0;
    REG_VRAMCNT_A = 0x80; // enabled, lcdc
    copyWords(__vram_copy, (vu32*)sFrame, VRAM_A_SIZE / 4);

    // engine A shows bank A as is on the top screen, engine B only its backdrop
    REG_POWCNT1 = sSaved.powcnt1 | 0x8203; // lcds, 2d A and B, A on top; 3d as it was
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
    REG_VRAMCNT_A = sSaved.vramcntA;
    PALETTE_B[0] = sSaved.backdropB;
    REG_MASTER_BRIGHT_A = sSaved.masterBrightA;
    REG_MASTER_BRIGHT_B = sSaved.masterBrightB;
    REG_POWCNT1 = sSaved.powcnt1;
    REG_DISPCNT_B = sSaved.dispcntB;
    REG_DISPCNT_A = sSaved.dispcntA;
    REG_DISPCAPCNT = sSaved.dispcapcnt;
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

static const struct
{
    const char* label;
    u32 state;
} sItems[] =
{
    { "Resume game", RTS_MENU_STATE_RESUME },
    { "Exit to launcher", RTS_MENU_STATE_EXIT },
};
#define ITEM_COUNT ((int)(sizeof(sItems) / sizeof(sItems[0])))
#define ITEMS_Y 64

static void drawGameLine(const u8* header)
{
    char line[33] = "Game: ";
    int n = 6;
    for (int i = 0; i < 12 && header[i]; i++)
        line[n++] = header[i];
    line[n++] = ' ';
    line[n++] = '(';
    for (int i = 0; i < 4; i++)
        line[n++] = header[12 + i];
    line[n++] = ')';
    line[n] = 0;
    drawText(8, 32, line, COLOR_DIM, COLOR_BG);
}

static void drawItems(int selected)
{
    for (int i = 0; i < ITEM_COUNT; i++)
    {
        int y = ITEMS_Y + i * 20;
        u16 paper = i == selected ? COLOR_SELECT_BG : COLOR_BG;
        u16 ink = i == selected ? COLOR_SELECT : COLOR_TEXT;
        fillRect(8, y - 2, SCREEN_W - 16, 20, paper);
        drawText(16, y, sItems[i].label, ink, paper);
    }
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

u32 rtsMain(void* block)
{
    takeScreens();
    fillRect(0, 0, SCREEN_W, SCREEN_H, COLOR_BG);
    fillRect(0, 0, SCREEN_W, 22, COLOR_TITLE_BG);
    drawText(8, 3, "Pico Loader RTS", COLOR_TEXT, COLOR_TITLE_BG);
    drawGameLine((const u8*)block + HEADER_FROM_BLOCK);
    drawText(8, SCREEN_H - 40, "Up/Down: choose  A: confirm", COLOR_DIM, COLOR_BG);
    drawText(8, SCREEN_H - 22, "B: back to the game", COLOR_DIM, COLOR_BG);

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
            selected = selected == 0 ? ITEM_COUNT - 1 : selected - 1;
        else if (keys & KEY_DOWN)
            selected = selected == ITEM_COUNT - 1 ? 0 : selected + 1;
        else if (keys & KEY_A)
            answer = sItems[selected].state;
        else if (keys & KEY_B)
            answer = RTS_MENU_STATE_RESUME;
        waitVBlank();
        drawItems(selected);
        waitRelease(); // one step per press, and the game should not see the key
    }

    giveScreensBack();
    return answer;
}
