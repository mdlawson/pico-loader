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

#define A_SHADOW        0x000
#define SH_MAGIC        0x31444853 // 'SHD1'
#define SH_FLAGS        0x128
#define A_MPU           0x280
#define A_POOL          0x500
#define V_SLOTS         0x11C
#define V_PATCHED       0x110
#define V_PATCHFAIL     0x114

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
    u32 perm = cp15Read(6);
    perm = (perm & ~V(v, V_NIB_MASK)) | nibble;
    cp15WritePermissions(perm);
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

int censusState(void)
{
    volatile u32* v = vars();
    return v ? (int)V(v, V_STATE) : 0;
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
        u32 st = sRts->censusStart;
        volatile u32* shd = st >= 0x02000000 && st < 0x02400000 && !(st & 3) ? (volatile u32*)st : 0;
        if (shd && shd[0] == 0x32444853 /* 'SHD2' */)
        {
            const volatile u8* flags = (const volatile u8*)st + 0x130;
            u32 written = 0;
            for (u32 i = 0; i < 0x120; i++)
                written += flags[i] != 0;
            put("Register shadow (no traps) at ");
            putHex(st, 8);
            put(": ");
            putDec(shd[2]);
            put(" store sites patched, ");
            putDec(written);
            put(" of 288 bytes written so far.");
            newline();
            return sLen;
        }
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

    // the register shadow: sites taken off the trap, and what it holds
    put("store sites patched off the trap: ");
    putDec(V(v, V_PATCHED));
    put(" of ");
    putDec(V(v, V_SLOTS));
    put(", no slot for ");
    putDec(V(v, V_PATCHFAIL));
    newline();
    u32 shown = 0;
    for (u32 i = 0; i < V(v, V_SLOTS) && i < 512 && shown < 32; i++)
    {
        const volatile u32* stub = a + (A_POOL + i * 24) / 4;
        if (!stub[0])
            continue;
        // the site from the stub's way back, the store as the stub has it
        u32 site = (u32)&stub[5] + 8 + ((int)(stub[5] << 8) >> 6) - 4;
        put(shown % 4 ? "  " : "  ");
        putHex(site, 8);
        put(":");
        putHex(stub[2], 8);
        if (++shown % 4 == 0)
            newline();
    }
    if (shown % 4)
        newline();
    volatile u32* sh = a + A_SHADOW / 4;
    put("shadow ");
    put(sh[0] == SH_MAGIC ? "on" : "missing");
    put(", registers the game wrote:");
    newline();
    const volatile u8* data = (const volatile u8*)(sh + 2);
    const volatile u8* flags = (const volatile u8*)sh + SH_FLAGS;
    shown = 0;
    for (u32 i = 0; i < 0x120; i += 2)
    {
        if (!flags[i] && !flags[i + 1])
            continue;
        u32 o = i < 0x48 ? 0x10 + i : i < 0x90 ? 0x1010 + (i - 0x48) : 0x330 + (i - 0x90);
        put(shown % 6 ? " " : "  ");
        putHex(o, 4);
        put("=");
        putHex(data[i] | (data[i + 1] << 8), 4);
        if (++shown % 6 == 0)
            newline();
    }
    if (shown % 6)
        newline();
    newline();
    put("Counts restart after each report; the calibration, patches and shadow stay.");
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
}
