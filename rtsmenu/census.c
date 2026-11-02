// The arm9 register-write census (arm9/source/patches/arm9/CensusPatchCode.s):
// keeps its traps off while the menu runs, turns them on and off, and writes
// what it counted to the log's fifth section.

#include "stateint.h"
#include "census.h"

// variables, as offsets into the census space
#define V_STATE         0x40
#define V_WANT          0x44
#define V_K             0x48
#define V_NIB_RO        0x60
#define V_NIB_RW        0x64
#define V_NIB_MASK      0x68
#define V_TRAPS         0x78
#define V_TRAPS_LAST    0x7C
#define V_FRAMES        0x80
#define V_MAXF          0x84
#define V_HIST          0x88
#define V_CAL_TRAP      0xA8
#define V_CAL_BASE      0xAC
#define V_CAL_N         0xB0
#define V_LINES         0xB4
#define V_STOP_REASON   0xB8
#define V_STOP_PC       0xBC
#define V_STOP_OP       0xC0
#define V_THUMB         0xC4
#define V_STM           0xC8
#define V_SITE_OVF      0xCC
#define V_CTRL          0xEC
#define V_INSTALLS      0xF0
#define V_ARRAYS        0xF4
#define V_MAGIC         0x108
#define CENSUS_MAGIC    0x31534E43 // 'CNS1'

#define A_SITES         0x000
#define SITE_COUNT      128
#define A_REGS          0x400
#define REG_COUNT       0x1DD
#define A_MPU           0xB80

#define LINE_CYCLES     4260    // arm9 cycles per scanline: 355 dots of 12
#define FRAME_LINES     263

static volatile u32* vars(void)
{
    u32 start = sRts->censusStart;
    // 0 or odd (the loader's note that there was no room) is no census
    if (start < 0x02000000 || start >= 0x02400000 || (start & 3))
        return 0;
    volatile u32* v = (volatile u32*)start;
    return v[V_MAGIC / 4] == CENSUS_MAGIC ? v : 0;
}

static u32 V(volatile u32* v, u32 offset)
{
    return v[offset / 4];
}

static void setNibble(volatile u32* v, u32 nibble)
{
    u32 perm;
    asm volatile("mrc p15, 0, %0, c5, c0, 2" : "=r"(perm));
    perm = (perm & ~V(v, V_NIB_MASK)) | nibble;
    asm volatile("mcr p15, 0, %0, c5, c0, 2" :: "r"(perm) : "memory");
}

int censusPresent(void)
{
    return vars() != 0;
}

int censusWanted(void)
{
    volatile u32* v = vars();
    return v && V(v, V_WANT);
}

void censusPause(void)
{
    volatile u32* v = vars();
    if (v && V(v, V_STATE) == 1)
        setNibble(v, V(v, V_NIB_RW));
}

void censusResume(void)
{
    volatile u32* v = vars();
    if (v && V(v, V_STATE) == 1 && V(v, V_WANT))
        setNibble(v, V(v, V_NIB_RO));
}

void censusToggle(void)
{
    volatile u32* v = vars();
    if (!v)
        return;
    if (V(v, V_WANT))
    {
        v[V_WANT / 4] = 0; // the next vblank takes the traps out
        return;
    }
    v[V_WANT / 4] = 1;
    if (V(v, V_STATE) == 3)
        v[V_STATE / 4] = 0; // stopped: try again
}

// ---------------------------------------------------------------- report

static char* sOut;
static u32 sLen, sMax;

static void put(const char* s)
{
    while (*s && sLen < sMax - 2)
        sOut[sLen++] = *s++;
}

static void putHex(u32 v, int digits)
{
    char t[9];
    for (int i = digits - 1; i >= 0; i--, v >>= 4)
        t[i] = "0123456789ABCDEF"[v & 15];
    t[digits] = 0;
    put(t);
}

static void putDec(u32 v)
{
    char t[11];
    int n = 0;
    do
        t[n++] = '0' + v % 10;
    while ((v /= 10) && n < 10);
    char o[11];
    for (int i = 0; i < n; i++)
        o[i] = t[n - 1 - i];
    o[n] = 0;
    put(o);
}

// tenths, as "12.3"
static void putTenths(u32 v)
{
    putDec(v / 10);
    char t[3] = { '.', (char)('0' + v % 10), 0 };
    put(t);
}

static void newline(void)
{
    put("\r\n");
}

static const char* regName(u32 o)
{
    static const struct { u16 from, to; const char* name; } names[] =
    {
        { 0x000, 0x003, "DISPCNT" },        { 0x004, 0x007, "DISPSTAT/VCOUNT" },
        { 0x008, 0x00F, "BGxCNT" },         { 0x010, 0x01F, "BG scroll" },
        { 0x020, 0x03F, "BG2/3 affine" },   { 0x040, 0x04B, "windows" },
        { 0x04C, 0x04F, "MOSAIC" },         { 0x050, 0x057, "blending" },
        { 0x060, 0x063, "DISP3DCNT" },      { 0x064, 0x067, "DISPCAPCNT" },
        { 0x068, 0x06B, "DISP_MMEM_FIFO" }, { 0x06C, 0x06F, "MASTER_BRIGHT" },
        { 0x0B0, 0x0DF, "DMA" },            { 0x0E0, 0x0EF, "DMA fill" },
        { 0x100, 0x10F, "timers" },         { 0x130, 0x137, "keys" },
        { 0x180, 0x183, "IPCSYNC" },        { 0x184, 0x187, "IPCFIFOCNT" },
        { 0x188, 0x18B, "IPCFIFOSEND" },    { 0x1A0, 0x1BF, "card" },
        { 0x204, 0x207, "EXMEMCNT" },       { 0x208, 0x20B, "IME" },
        { 0x210, 0x213, "IE" },             { 0x214, 0x217, "IF" },
        { 0x240, 0x24B, "VRAMCNT/WRAMCNT" },{ 0x280, 0x2BF, "divide/sqrt" },
        { 0x300, 0x303, "POSTFLG" },        { 0x304, 0x307, "POWCNT1" },
        { 0x320, 0x3FF, "3D settings" },    { 0x400, 0x43F, "GXFIFO" },
        { 0x440, 0x5FF, "3D commands" },    { 0x600, 0x6FF, "3D status/results" },
    };
    for (u32 i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (o >= names[i].from && o <= names[i].to)
            return names[i].name;
    return "";
}

static u32 regOffset(u32 index)
{
    if (index < 0x1C0)
        return index * 4;
    if (index < 0x1DC)
        return 0x1000 + (index - 0x1C0) * 4;
    return 0xFFFFFFFF;
}

static void putReg(u32 index)
{
    u32 o = regOffset(index);
    if (o == 0xFFFFFFFF)
    {
        put("other io");
        return;
    }
    put("04");
    putHex(0x000000 + o, 6);
    put(" ");
    if (o >= 0x1000)
        put("B ");
    put(regName(o & 0xFFF));
}

// into a 33 byte line
static void appendText(char* line, u32* n, const char* s)
{
    while (*s && *n < 32)
        line[(*n)++] = *s++;
    line[*n] = 0;
}

static void appendNumber(char* line, u32* n, u32 v)
{
    char t[11];
    int d = 0;
    do
        t[d++] = '0' + v % 10;
    while ((v /= 10) && d < 10);
    while (d && *n < 32)
        line[(*n)++] = t[--d];
    line[*n] = 0;
}

static const char* const sStates[] = { "off", "trapping", "no region it can use", "stopped" };
static const char* const sReasons[] =
{
    "", "a store it does not handle", "an abort that is not an io store",
    "the game changed the protection unit or the vector", "the game's cpu mode",
};

// Into out (size bytes), after the log's title and game line; returns the
// length. The summary for the menu's two status lines into line1 and line2
// (33 bytes each).
u32 censusReport(char* out, u32 size, char* line1, char* line2)
{
    sOut = out;
    sLen = 0;
    sMax = size;
    line1[0] = line2[0] = 0;
    volatile u32* v = vars();
    if (!v)
    {
        put("No census in this boot: it needs the exit combo, an ntr sdk game and room in the");
        newline();
        put("arm7's main memory arena.");
        newline();
        u32 start = sRts->censusStart;
        if (start & 1)
        {
            put("The loader found no room: the arm7's main memory arena starts at ");
            putHex(start & ~3u, 8);
            put(".");
            newline();
        }
        put("rts info: census at ");
        putHex(start, 8);
        if (start >= 0x02000000 && start < 0x02400000 && !(start & 3))
        {
            put(", magic there ");
            putHex(((volatile u32*)start)[V_MAGIC / 4], 8);
        }
        put(", arm7 patch space ");
        putHex(sRts->arm7PatchStart, 8);
        put(" size ");
        putHex(sRts->arm7PatchSize, 4);
        newline();
        return sLen;
    }
    u32 state = V(v, V_STATE);
    u32 frames = V(v, V_FRAMES);
    u32 traps = V(v, V_TRAPS);
    put("census at ");
    putHex(sRts->censusStart, 8);
    put(", state ");
    put(state < 4 ? sStates[state] : "?");
    put(V(v, V_WANT) ? " (on)" : " (turned off)");
    put(", installs ");
    putDec(V(v, V_INSTALLS));
    newline();
    if (V(v, V_STOP_REASON))
    {
        put("stopped by ");
        u32 r = V(v, V_STOP_REASON);
        put(r < 5 ? sReasons[r] : "?");
        put(" at ");
        putHex(V(v, V_STOP_PC), 8);
        put(", opcode ");
        putHex(V(v, V_STOP_OP), 8);
        newline();
    }
    volatile u32* a = (volatile u32*)V(v, V_ARRAYS);
    put("protection unit at install: regions");
    for (int i = 0; i < 8; i++)
    {
        put(" ");
        putHex(a[A_MPU / 4 + i], 8);
    }
    newline();
    put("  data perm ");
    putHex(a[A_MPU / 4 + 8], 8);
    put(", insn perm ");
    putHex(a[A_MPU / 4 + 9], 8);
    put(", dcache ");
    putHex(a[A_MPU / 4 + 10], 2);
    put(", icache ");
    putHex(a[A_MPU / 4 + 11], 2);
    put(", wbuf ");
    putHex(a[A_MPU / 4 + 12], 2);
    put(", control ");
    putHex(V(v, V_CTRL), 8);
    put(", region used ");
    putDec(V(v, V_K));
    newline();
    newline();

    // calibration: CAL_N writes of DISPCNT, trapped and not
    u32 calN = V(v, V_CAL_N);
    u32 cyclesPerTrap = 0;
    if (calN)
    {
        u32 trapLines = V(v, V_CAL_TRAP), baseLines = V(v, V_CAL_BASE);
        cyclesPerTrap = trapLines > baseLines ? (trapLines - baseLines) * LINE_CYCLES / calN : 0;
        put("calibration: ");
        putDec(calN);
        put(" writes took ");
        putDec(trapLines);
        put(" lines trapped, ");
        putDec(baseLines);
        put(" not: about ");
        putDec(cyclesPerTrap);
        put(" arm9 cycles per trap (1 line = 4260)");
        newline();
    }

    put("frames ");
    putDec(frames);
    put(", traps ");
    putDec(traps);
    put(" (thumb ");
    putDec(V(v, V_THUMB));
    put(", store multiple ");
    putDec(V(v, V_STM));
    put(")");
    newline();
    if (frames)
    {
        u32 avg10 = (u32)((u64)traps * 10 / frames);
        u32 maxf = V(v, V_MAXF);
        put("per frame: ");
        putTenths(avg10);
        put(" traps on average, ");
        putDec(maxf);
        put(" at most");
        newline();
        // cost as a share of a frame (1120380 cycles), in tenths of a percent
        u32 avgCost = (u32)((u64)traps * cyclesPerTrap * 1000 / ((u64)frames * LINE_CYCLES * FRAME_LINES));
        u32 maxCost = (u32)((u64)maxf * cyclesPerTrap * 1000 / ((u64)LINE_CYCLES * FRAME_LINES));
        put("cost from the calibration: ");
        putTenths(avgCost);
        put("% of a frame on average, ");
        putTenths(maxCost);
        put("% in the busiest frame");
        newline();
        u32 lines = V(v, V_LINES);
        u32 sampled = (u32)((u64)lines * 1000 / ((u64)frames * FRAME_LINES));
        put("time inside the handler (lines crossed, bios entry not included): ");
        putTenths(sampled);
        put("%");
        newline();
        put("frames by traps: 0: ");
        static const char* const buckets[] = { "", "1-15", "16-63", "64-255", "256-1023", "1024-4095", "4096-16383", "16384+" };
        putDec(V(v, V_HIST));
        for (int i = 1; i < 8; i++)
        {
            put(", ");
            put(buckets[i]);
            put(": ");
            putDec(V(v, V_HIST + 4 * i));
        }
        newline();

        // the menu's two lines
        u32 n = 0;
        appendText(line1, &n, "Traps/frame: avg ");
        appendNumber(line1, &n, avg10 / 10);
        appendText(line1, &n, ", max ");
        appendNumber(line1, &n, maxf);
        n = 0;
        appendText(line2, &n, "Cost: ");
        appendNumber(line2, &n, avgCost / 10);
        appendText(line2, &n, ".");
        appendNumber(line2, &n, avgCost % 10);
        appendText(line2, &n, "% avg, ");
        appendNumber(line2, &n, maxCost / 10);
        appendText(line2, &n, "% max");
    }
    newline();

    // sites, busiest first
    put("busiest store sites (address, t = thumb, traps):");
    newline();
    u32 shown = 0, siteTotal = 0, sites = 0;
    for (int i = 0; i < SITE_COUNT; i++)
        if (a[A_SITES / 4 + 2 * i])
        {
            sites++;
            siteTotal += a[A_SITES / 4 + 2 * i + 1];
        }
    u32 last = 0xFFFFFFFF;
    while (shown < 24)
    {
        u32 best = 0, bestAt = 0xFFFFFFFF;
        for (int i = 0; i < SITE_COUNT; i++)
        {
            u32 n = a[A_SITES / 4 + 2 * i + 1];
            if (a[A_SITES / 4 + 2 * i] && n < last && n > best)
            {
                best = n;
                bestAt = i;
            }
        }
        if (bestAt == 0xFFFFFFFF)
            break;
        // every site with this count
        for (int i = 0; i < SITE_COUNT && shown < 24; i++)
        {
            if (a[A_SITES / 4 + 2 * i] && a[A_SITES / 4 + 2 * i + 1] == best)
            {
                u32 pc = a[A_SITES / 4 + 2 * i];
                put("  ");
                putHex(pc & ~1u, 8);
                put(pc & 1 ? " t " : "   ");
                putDec(best);
                newline();
                shown++;
            }
        }
        last = best;
    }
    put("  sites ");
    putDec(sites);
    put(", their traps ");
    putDec(siteTotal);
    put(", traps from sites past the table ");
    putDec(V(v, V_SITE_OVF));
    newline();
    newline();

    // registers, busiest first
    put("busiest registers (word, traps or words written):");
    newline();
    shown = 0;
    last = 0xFFFFFFFF;
    while (shown < 40)
    {
        u32 best = 0;
        for (int i = 0; i < REG_COUNT; i++)
        {
            u32 n = a[A_REGS / 4 + i];
            if (n < last && n > best)
                best = n;
        }
        if (!best)
            break;
        for (int i = 0; i < REG_COUNT && shown < 40; i++)
        {
            if (a[A_REGS / 4 + i] == best)
            {
                put("  ");
                putReg(i);
                put(": ");
                putDec(best);
                newline();
                shown++;
            }
        }
        last = best;
    }
    newline();
    put("Counts restart after each report; the calibration stays.");
    newline();
    return sLen;
}

void censusRestart(void)
{
    volatile u32* v = vars();
    if (!v)
        return;
    v[V_TRAPS / 4] = 0;
    v[V_TRAPS_LAST / 4] = 0;
    v[V_FRAMES / 4] = 0;
    v[V_MAXF / 4] = 0;
    for (int i = 0; i < 8; i++)
        v[V_HIST / 4 + i] = 0;
    v[V_LINES / 4] = 0;
    v[V_THUMB / 4] = 0;
    v[V_STM / 4] = 0;
    v[V_SITE_OVF / 4] = 0;
    volatile u32* a = (volatile u32*)V(v, V_ARRAYS);
    for (int i = 0; i < A_MPU / 4; i++)
        a[i] = 0;
}
