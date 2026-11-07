// ioprof.h - the arm9's cpu writes to the io pages a trap would catch (0x04000000-0x04001FFF),
// by code site: what a write-trapping save state would cost, and which sites a patch could take
// off the trap. IOPROF=path turns it on; IOPROF_FROM=frame starts counting.
#pragma once
#include <map>
#include <string>
namespace ioprof {
using namespace melonDS;
static NDS* gNds; static bool on; static int from; static int frame;
struct Site { u64 traps = 0; u32 classes = 0; bool thumb = false; u32 op = 0; u32 lastAddr = 0; };
static std::map<u32, Site> sites;
static u32 lastPc = ~0u, lastAddr = 0; static u64 lastCyc = 0;
static u64 frameTraps = 0, maxFrame = 0, totalFrames = 0, total = 0;
static u64 classTraps[8];
// 0 interesting (2d/3d write-only settings), 1 safe to patch (3d ports, divider, irq, card, ipc), 2 other
static int cls(u32 a)
{
    a &= 0x0FFFFFFF;
    if (a >= 0x04001000) { u32 o = a - 0x04001000; return (o >= 0x10 && o < 0x56) ? 0 : 2; }
    u32 o = a - 0x04000000;
    if ((o >= 0x10 && o < 0x56) || (o >= 0x330 && o < 0x3C0)) return 0;
    if ((o >= 0x400 && o < 0x700) || (o >= 0x280 && o < 0x2C0) || (o >= 0x208 && o < 0x218) || (o >= 0x1A0 && o < 0x1C0) || (o >= 0x180 && o < 0x190)) return 1;
    return 2;
}
static u32 watchLo = 0, watchHi = 0;
// SHADOWLOG=path: every distinct (site, register) that writes a register the shadow keeps
struct SKey { u32 pc, addr; bool operator<(const SKey& o) const { return pc != o.pc ? pc < o.pc : addr < o.addr; } };
struct SVal { u64 n = 0; int first = 0, last = 0; u32 op = 0; };
static std::map<SKey, SVal> shadowLog; static bool shadowOn;
// SHADOWLOG_FROM=frame: count from there; SHADOWLOG.sum gets stub hits per frame
static int shFrom; static u32 shLastPc = ~0u, shLastAddr;
static u64 shFrameHits, shFrameDma, shFrameActive, shHits, shDma, shActive, shMax, shFrames; static int shMaxFrame;
static bool shadowed(u32 a) { u32 o = a & 0x0FFFFFFF; if (o >= 0x04001000 && o < 0x04002000) o -= 0x1000; else if (o >= 0x04002000) return false; o -= 0x04000000; return (o >= 0x10 && o < 0x58) || (a < 0x04001000 && o >= 0x330 && o < 0x3C0); }
// RTS_SCRAMBLE with "written": which shadowed bytes the game has ever changed from their
// reset value (by cpu or dma). The rest hold the same in every session of the game.
static u8 hVal[0x120], hChanged[0x120]; static bool trackOn, trackPaused;
static int shIndex(u32 a) { u32 o = a & 0x0FFFFFFF; if (o >= 0x04001010 && o < 0x04001058) return o - 0x04001010 + 0x48; if (o >= 0x04000010 && o < 0x04000058) return o - 0x04000010; if (o >= 0x04000330 && o < 0x040003C0) return o - 0x04000330 + 0x90; return -1; }
static void hook(u32 addr, u32 val, int size)
{
    if (trackOn && !trackPaused)
        for (int b = 0; b < size; b++)
        {
            int i = shIndex(addr + b);
            if (i < 0) continue;
            u8 x = val >> (8 * b);
            // the loader (it runs from vram) sets some up the same way every boot
            bool loader = !melonDS::gHarnessInDMA && (gNds->ARM9.R[15] >> 24) == 0x06;
            // and anything before it (the launcher) is gone by the time the game runs
            if (loader) memset(hChanged, 0, sizeof(hChanged));
            else if (x != hVal[i]) hChanged[i] = 1;
            hVal[i] = x;
        }
    if (shadowOn && shadowed(addr) && frame >= shFrom)
    {
        bool th = gNds->ARM9.CPSR & 0x20;
        u32 pc = melonDS::gHarnessInDMA ? 0xD0000000u : (gNds->ARM9.R[15] - (th ? 4 : 8)) | th;
        // a stub's worth: one store (the words of a strd/stm are one), dma apart
        if (melonDS::gHarnessInDMA) shFrameDma++;
        else if (!(pc == shLastPc && addr == shLastAddr + 4)) shFrameHits++;
        shLastPc = pc; shLastAddr = addr;
        int line = gNds->GPU.VCount;
        if (!melonDS::gHarnessInDMA && line < 192) shFrameActive++;
        SVal& v = shadowLog[{pc, addr & ~1u}];
        if (!v.n) { v.first = frame; v.op = melonDS::gHarnessInDMA ? 0 : th ? gNds->ARM9Read16(pc & ~1u) : gNds->ARM9Read32(pc); }
        v.n++; v.last = frame;
    }
    if (watchHi && (addr & 0x0FFFFFFF) >= watchLo && (addr & 0x0FFFFFFF) < watchHi)
    {
        bool th = gNds->ARM9.CPSR & 0x20;
        fprintf(stderr, "iowatch f%d %08X=%08X s%d pc %08X lr %08X r0 %08X r1 %08X%s\n", frame, addr, val, size, gNds->ARM9.R[15] - (th ? 4 : 8), gNds->ARM9.R[14], gNds->ARM9.R[0], gNds->ARM9.R[1], melonDS::gHarnessInDMA ? " DMA" : "");
    }
    if (!on || melonDS::gHarnessInDMA) return;
    if (frame < from || (addr & 0x0FFFFFFF) >= 0x04002000) return;
    bool thumb = gNds->ARM9.CPSR & 0x20;
    u32 pc = gNds->ARM9.R[15] - (thumb ? 4 : 8);
    // the words of one store multiple (or a strd) count as one trap
    bool same = pc == lastPc && addr == lastAddr + 4;
    lastPc = pc; lastAddr = addr;
    Site& s = sites[pc | thumb];
    int c = cls(addr);
    s.classes |= 1u << c; s.thumb = thumb; if (c != 1) s.lastAddr = addr;
    if (same) return;
    s.traps++; frameTraps++; total++; classTraps[c]++;
    if (!s.op) s.op = thumb ? gNds->ARM9Read16(pc) : gNds->ARM9Read32(pc);
}
// SHADOW7LOG=path: the same for the arm7's sound channel SAD/TMR/PNT/LEN, timer
// reloads and capture registers
static std::map<SKey, SVal> shadow7Log;
static bool shadowed7(u32 a) { u32 o = (a & 0x0FFFFFFF) - 0x04000000; if (o >= 0x400 && o < 0x500) return (o & 0xF) >= 4; if (o >= 0x100 && o < 0x110) return (o & 3) < 2; return o >= 0x510 && o < 0x520; }
static void hook7(u32 addr, u32 val, int size)
{
    for (int b = 0; b < size; b += 2)
    {
        if (!shadowed7(addr + b)) continue;
        bool th = gNds->ARM7.CPSR & 0x20;
        u32 pc = melonDS::gHarnessInDMA ? 0xD0000000u : (gNds->ARM7.R[15] - (th ? 4 : 8)) | th;
        SVal& v = shadow7Log[{pc, (addr + b) & ~1u}];
        if (!v.n) { v.first = frame; v.op = melonDS::gHarnessInDMA ? 0 : th ? gNds->ARM7Read16(pc & ~1u) : gNds->ARM7Read32(pc); }
        v.n++; v.last = frame;
    }
}
static void start(NDS* n) { gNds = n; if (getenv("SHADOW7LOG")) melonDS::gHarnessIO7Write = hook7; if (getenv("RTS_SCRAMBLE") && strstr(getenv("RTS_SCRAMBLE"), "written")) { trackOn = true; melonDS::gHarnessIOWrite = hook; } if (getenv("SHADOWLOG")) { shadowOn = true; shFrom = getenv("SHADOWLOG_FROM") ? atoi(getenv("SHADOWLOG_FROM")) : 0; melonDS::gHarnessIOWrite = hook; } if (const char* w = getenv("IOWATCH")) { sscanf(w, "%x-%x", &watchLo, &watchHi); if (!watchHi) watchHi = watchLo + 4; melonDS::gHarnessIOWrite = hook; } const char* e = getenv("IOPROF"); if (!e) return; on = true; from = getenv("IOPROF_FROM") ? atoi(getenv("IOPROF_FROM")) : 0; melonDS::gHarnessIOWrite = hook; }
static void endFrame(int f)
{
    frame = f + 1;
    if (shadowOn && f >= shFrom)
    {
        shFrames++; shHits += shFrameHits; shDma += shFrameDma; shActive += shFrameActive;
        if (shFrameHits > shMax) { shMax = shFrameHits; shMaxFrame = f; }
        shFrameHits = shFrameDma = shFrameActive = 0;
    }
    if (!on || f < from) return;
    totalFrames++; if (frameTraps > maxFrame) maxFrame = frameTraps; frameTraps = 0;
}
static void report()
{
    if (shadowOn) if (FILE* o = fopen(getenv("SHADOWLOG"), "w"))
    {
        for (auto& [k, v] : shadowLog) fprintf(o, "%08X %08X %08X %llu %d %d\n", k.pc, k.addr, v.op, (unsigned long long)v.n, v.first, v.last);
        fclose(o);
    }
    if (shadowOn) if (FILE* o = fopen((std::string(getenv("SHADOWLOG")) + ".sum").c_str(), "w"))
    {
        double pf = shFrames ? 1.0 / shFrames : 0;
        fprintf(o, "frames %llu cpu stores/frame %.1f (in active lines %.1f) max %llu at frame %d; dma writes/frame %.1f\n",
            (unsigned long long)shFrames, shHits * pf, shActive * pf, (unsigned long long)shMax, shMaxFrame, shDma * pf);
        fclose(o);
    }
    if (getenv("SHADOW7LOG")) if (FILE* o = fopen(getenv("SHADOW7LOG"), "w"))
    {
        for (auto& [k, v] : shadow7Log) fprintf(o, "%08X %08X %08X %llu %d %d\n", k.pc, k.addr, v.op, (unsigned long long)v.n, v.first, v.last);
        fclose(o);
    }
    if (!on) return;
    FILE* o = fopen(getenv("IOPROF"), "w"); if (!o) return;
    fprintf(o, "frames %llu traps %llu per frame %.1f max %llu\n", (unsigned long long)totalFrames, (unsigned long long)total, totalFrames ? (double)total / totalFrames : 0, (unsigned long long)maxFrame);
    fprintf(o, "by target: interesting %llu, patchable %llu, other %llu\n", (unsigned long long)classTraps[0], (unsigned long long)classTraps[1], (unsigned long long)classTraps[2]);
    // left after patching: thumb sites, sites with interesting or other targets, sites that mix
    u64 leftThumb = 0, leftMixed = 0, leftOther = 0, leftInt = 0, patchedArm = 0; int nPatch = 0;
    for (auto& [pc, s] : sites)
    {
        if (s.classes == 2u && !s.thumb) { patchedArm += s.traps; nPatch++; }
        else if (s.classes == 2u) leftThumb += s.traps;
        else if (s.classes & 2u) leftMixed += s.traps;
        else if (s.classes & 1u) leftInt += s.traps;
        else leftOther += s.traps;
    }
    double pf = totalFrames ? 1.0 / totalFrames : 0;
    fprintf(o, "patchable arm sites %d take %.1f traps/frame off; left per frame: thumb patchable %.1f, mixed %.1f, interesting %.1f, other %.1f\n",
        nPatch, patchedArm * pf, leftThumb * pf, leftMixed * pf, leftInt * pf, leftOther * pf);
    std::vector<std::pair<u64, u32>> v; for (auto& [pc, s] : sites) v.push_back({s.traps, pc});
    std::sort(v.rbegin(), v.rend());
    fprintf(o, "sites %zu; busiest (pc t traps classes op):\n", sites.size());
    for (size_t i = 0; i < v.size() && i < 40; i++) { auto& s = sites[v[i].second]; fprintf(o, "  %08X %c %8llu %c%c%c %08X\n", v[i].second & ~1u, s.thumb ? 't' : ' ', (unsigned long long)s.traps, s.classes & 1 ? 'I' : '-', s.classes & 2 ? 'P' : '-', s.classes & 4 ? 'O' : '-', s.op); }
    std::map<u32, u64> otherRegs; for (auto& [pc, s] : sites) if (s.classes & 4) otherRegs[s.lastAddr & ~1u] += s.traps;
    fprintf(o, "other registers (by a site's last one):"); for (auto& [a, n] : otherRegs) fprintf(o, " %08X:%llu", a, (unsigned long long)n); fprintf(o, "\n");
    fprintf(o, "sites writing interesting registers (last address):\n");
    for (auto& [pc, s] : sites) if (s.classes & 1) fprintf(o, "  %08X %c %8llu %c%c%c %08X %08X\n", pc & ~1u, s.thumb ? 't' : ' ', (unsigned long long)s.traps, s.classes & 1 ? 'I' : '-', s.classes & 2 ? 'P' : '-', s.classes & 4 ? 'O' : '-', s.op, s.lastAddr);
    fclose(o);
}
}
