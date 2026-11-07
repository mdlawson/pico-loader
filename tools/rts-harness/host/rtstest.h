#include <functional>
// rtstest.h: drives the RTS menu's own restore test from dshost and compares the
// two runs it makes (after "Test: save + reference" and "Test: load + compare")
// with what the emulator knows, including the registers a DS can't read back.
//
// The arm7 counts rts_info_t.testFrames down from TEST_FRAMES on vblanks after
// each resume, so both runs are lined up by that count: frame k of the
// reference is compared with frame k after the load.
#pragma once
#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include "NDS.h"
#include "GPU.h"
#include "SPU.h"
#include "Savestate.h"
#include <set>
#include <memory>

namespace rtstest {
using namespace melonDS;

static const u32 RTS_INFO_MAGIC = 0x31535452, RTS_BASE = 0x02100000;
static const u32 TEST_FRAMES = 120;
static const u32 K_A = 1 << 0, K_B = 1 << 1, K_SELECT = 1 << 2, K_RIGHT = 1 << 4, K_UP = 1 << 6,
                 K_DOWN = 1 << 7, K_R = 1 << 8, K_L = 1 << 9;

typedef std::vector<std::pair<std::string, u64>> Snap;

static void add(Snap& s, const std::string& n, u64 v) { s.push_back({n, v}); }
template <typename T> static void addArr(Snap& s, const std::string& n, const T* a, int count)
{
    for (int i = 0; i < count; i++) add(s, n + "[" + std::to_string(i) + "]", (u64)(s64)a[i]);
}

// The hardware state a save state has to get back. Not the running counters
// (timer counts, scanline position), which a correct load gets back anyway.
static Snap snapshot(NDS& nds)
{
    Snap s;
    GPU& g = nds.GPU;
    for (int u = 0; u < 2; u++)
    {
        GPU2D& e = u ? g.GPU2D_B : g.GPU2D_A;
        std::string p = u ? "2dB." : "2dA.";
        add(s, p + "DispCnt", e.DispCnt);
        addArr(s, p + "BGCnt", e.BGCnt, 4);
        addArr(s, p + "BGHOFS", e.BGXPos, 4);
        addArr(s, p + "BGVOFS", e.BGYPos, 4);
        addArr(s, p + "BGX(affine ref)", e.BGXRef, 2);
        addArr(s, p + "BGY(affine ref)", e.BGYRef, 2);
        addArr(s, p + "BGPA", e.BGRotA, 2);
        addArr(s, p + "BGPB", e.BGRotB, 2);
        addArr(s, p + "BGPC", e.BGRotC, 2);
        addArr(s, p + "BGPD", e.BGRotD, 2);
        addArr(s, p + "WIN0(x1,x2,y1,y2)", e.Win0Coords, 4);
        addArr(s, p + "WIN1(x1,x2,y1,y2)", e.Win1Coords, 4);
        addArr(s, p + "WINCNT", e.WinCnt, 4);
        addArr(s, p + "MOSAIC.bg", e.BGMosaicSize, 2);
        addArr(s, p + "MOSAIC.obj", e.OBJMosaicSize, 2);
        add(s, p + "BLDCNT", e.BlendCnt);
        add(s, p + "BLDALPHA", e.BlendAlpha);
        add(s, p + "BLDY", e.EVY);
    }
    add(s, "MASTER_BRIGHT.A", g.MasterBrightnessA);
    add(s, "MASTER_BRIGHT.B", g.MasterBrightnessB);
    add(s, "DISPCAPCNT", g.CaptureCnt);
    add(s, "DISPSTAT9", g.DispStat[0]);
    add(s, "DISPSTAT7", g.DispStat[1]);
    addArr(s, "VRAMCNT", g.VRAMCNT, 9);

    GPU3D& d = g.GPU3D;
    add(s, "3d.DISP3DCNT", d.DispCnt);
    add(s, "3d.ALPHA_TEST_REF", d.AlphaRef);
    addArr(s, "3d.TOON_TABLE", d.ToonTable, 32);
    addArr(s, "3d.EDGE_COLOR", d.EdgeTable, 8);
    add(s, "3d.FOG_COLOR", d.FogColor);
    add(s, "3d.FOG_OFFSET", d.FogOffset);
    addArr(s, "3d.FOG_TABLE", d.FogDensityTable, 32);
    add(s, "3d.CLEAR_COLOR", d.ClearAttr1);
    add(s, "3d.CLEAR_DEPTH", d.ClearAttr2);
    add(s, "3d.MatrixMode", d.MatrixMode);
    addArr(s, "3d.ProjMatrix", d.ProjMatrix, 16);
    addArr(s, "3d.PosMatrix", d.PosMatrix, 16);
    addArr(s, "3d.VecMatrix", d.VecMatrix, 16);
    addArr(s, "3d.TexMatrix", d.TexMatrix, 16);
    addArr(s, "3d.Viewport", d.Viewport, 6);
    add(s, "3d.ProjStackPtr", d.ProjMatrixStackPointer);
    add(s, "3d.PosStackPtr", d.PosMatrixStackPointer);
    add(s, "3d.TexStackPtr", d.TexMatrixStackPointer);
    addArr(s, "3d.ProjStack", d.ProjMatrixStack, 16);
    addArr(s, "3d.TexStack", d.TexMatrixStack, 16);
    for (int i = 0; i < 32; i++) addArr(s, "3d.PosStack" + std::to_string(i), d.PosMatrixStack[i], 16);
    for (int i = 0; i < 32; i++) addArr(s, "3d.VecStack" + std::to_string(i), d.VecMatrixStack[i], 16);
    for (int i = 0; i < 4; i++) addArr(s, "3d.LightDir" + std::to_string(i), d.LightDirection[i], 3);
    for (int i = 0; i < 4; i++) addArr(s, "3d.LightColor" + std::to_string(i), d.LightColor[i], 3);
    addArr(s, "3d.MatDiffuse", d.MatDiffuse, 3);
    addArr(s, "3d.MatAmbient", d.MatAmbient, 3);
    addArr(s, "3d.MatSpecular", d.MatSpecular, 3);
    addArr(s, "3d.MatEmission", d.MatEmission, 3);
    add(s, "3d.UseShininess", d.UseShininessTable);
    addArr(s, "3d.Shininess", d.ShininessTable, 128);
    add(s, "3d.PolygonAttr", d.PolygonAttr);
    add(s, "3d.TexParam", d.TexParam);
    add(s, "3d.TexPalette", d.TexPalette);
    add(s, "3d.PolygonMode", d.PolygonMode);
    addArr(s, "3d.VertexColor", d.VertexColor, 3);

    for (int i = 0; i < 8; i++)
    {
        std::string p = std::string(i < 4 ? "dma9." : "dma7.") + std::to_string(i & 3);
        add(s, p + ".SAD", nds.HarnessDMA(i).SrcAddr);
        add(s, p + ".DAD", nds.HarnessDMA(i).DstAddr);
        add(s, p + ".CNT", nds.HarnessDMA(i).Cnt);
    }
    for (int i = 0; i < 8; i++)
    {
        std::string p = std::string(i < 4 ? "tm9." : "tm7.") + std::to_string(i & 3);
        add(s, p + ".reload", nds.Timers[i].Reload);
        add(s, p + ".CNT", nds.Timers[i].Cnt);
    }
    add(s, "IME9", nds.IME[0]); add(s, "IME7", nds.IME[1]);
    add(s, "IE9", nds.IE[0]); add(s, "IE7", nds.IE[1]);
    add(s, "EXMEMCNT9", nds.ExMemCnt[0]); add(s, "EXMEMCNT7", nds.ExMemCnt[1]);
    add(s, "POWCNT1", nds.PowerControl9);

    SPU& spu = nds.SPU;
    add(s, "snd.SOUNDCNT", spu.HarnessCnt());
    add(s, "snd.SOUNDBIAS", spu.HarnessBias());
    for (int i = 0; i < 16; i++)
    {
        SPUChannel& c = spu.HarnessChannel(i);
        std::string p = "snd" + std::to_string(i) + ".";
        add(s, p + "CNT", c.Cnt);
        // only what the game wrote matters when the channel is off
        add(s, p + "SAD", c.SrcAddr);
        add(s, p + "TMR", c.TimerReload);
        add(s, p + "PNT", c.LoopPos);
        add(s, p + "LEN", c.Length);
    }
    return s;
}

struct Frame { u32 hash; std::vector<u32> pixels; Snap snap; std::vector<u8> ram; std::vector<s16> audio; };

// Experiments: a component's emulator state, taken as the game resumes after the
// save and put back while the menu runs after the load, to see what a load
// would need to get right. Names: 3d, spu, 2d, dma.
static void doComponent(NDS& nds, const std::string& c, Savestate* f)
{
    if (c == "3d") nds.GPU.GPU3D.DoSavestate(f);
    else if (c == "spu") nds.SPU.DoSavestate(f);
    else if (c == "2d") { nds.GPU.GPU2D_A.DoSavestate(f); nds.GPU.GPU2D_B.DoSavestate(f); }
    else if (c == "dma") for (int i = 0; i < 8; i++) nds.HarnessDMA(i).DoSavestate(f);
}
// Finer 3d groups, copied field by field (the savestate takes all of GPU3D).
template <typename T> static void cp(std::vector<u8>& buf, size_t& at, T& v, bool take)
{
    if (take) { buf.resize(at + sizeof(T)); memcpy(buf.data() + at, &v, sizeof(T)); }
    else memcpy(&v, buf.data() + at, sizeof(T));
    at += sizeof(T);
}
static bool fields3d(NDS& nds, const std::string& c, std::vector<u8>& buf, bool take)
{
    GPU3D& d = nds.GPU.GPU3D;
    size_t at = 0;
    if (c == "3dmtx")
    {
        cp(buf, at, d.ProjMatrix, take); cp(buf, at, d.PosMatrix, take); cp(buf, at, d.VecMatrix, take);
        cp(buf, at, d.TexMatrix, take); cp(buf, at, d.ProjMatrixStack, take); cp(buf, at, d.PosMatrixStack, take);
        cp(buf, at, d.VecMatrixStack, take); cp(buf, at, d.TexMatrixStack, take); cp(buf, at, d.ProjMatrixStackPointer, take);
        cp(buf, at, d.PosMatrixStackPointer, take); cp(buf, at, d.TexMatrixStackPointer, take); cp(buf, at, d.MatrixMode, take);
        if (!take) d.ClipMatrixDirty = true;
        return true;
    }
    if (c == "3dcur") // just the current matrices, no stacks, no mode
    {
        cp(buf, at, d.ProjMatrix, take); cp(buf, at, d.PosMatrix, take); cp(buf, at, d.VecMatrix, take);
        if (!take) d.ClipMatrixDirty = true;
        return true;
    }
    if (c == "3dtex") { cp(buf, at, d.TexMatrix, take); return true; }
    if (c == "3dmode") { cp(buf, at, d.MatrixMode, take); return true; }
    if (c == "3dset")
    {
        cp(buf, at, d.AlphaRefVal, take); cp(buf, at, d.AlphaRef, take); cp(buf, at, d.ToonTable, take);
        cp(buf, at, d.EdgeTable, take); cp(buf, at, d.FogColor, take); cp(buf, at, d.FogOffset, take);
        cp(buf, at, d.FogDensityTable, take); cp(buf, at, d.ClearAttr1, take); cp(buf, at, d.ClearAttr2, take);
        return true;
    }
    if (c == "3dlight")
    {
        cp(buf, at, d.LightDirection, take); cp(buf, at, d.LightColor, take); cp(buf, at, d.MatDiffuse, take);
        cp(buf, at, d.MatAmbient, take); cp(buf, at, d.MatSpecular, take); cp(buf, at, d.MatEmission, take);
        cp(buf, at, d.UseShininessTable, take); cp(buf, at, d.ShininessTable, take); cp(buf, at, d.SpecRecip, take);
        cp(buf, at, d.PolygonAttr, take); cp(buf, at, d.TexParam, take); cp(buf, at, d.TexPalette, take);
        cp(buf, at, d.Viewport, take); cp(buf, at, d.VertexColor, take); cp(buf, at, d.PolygonMode, take);
        return true;
    }
    return false;
}

// "sndrestart": what a load could do with the channel settings alone: start
// the channels that were playing again from their start
struct ChanRegs { u32 cnt, sad, tmr, pnt, len; };
static std::vector<u8> takeChannels(NDS& nds)
{
    std::vector<u8> b(16 * sizeof(ChanRegs));
    ChanRegs* r = (ChanRegs*)b.data();
    for (int i = 0; i < 16; i++) { SPUChannel& c = nds.SPU.HarnessChannel(i); r[i] = { c.Cnt, c.SrcAddr, c.TimerReload, c.LoopPos, c.Length }; }
    return b;
}
static void restartChannels(NDS& nds, std::vector<u8>& b)
{
    ChanRegs* r = (ChanRegs*)b.data();
    for (int i = 0; i < 16; i++)
    {
        SPUChannel& c = nds.SPU.HarnessChannel(i);
        if (!(r[i].cnt & 0x80000000) || (c.Cnt & 0x80000000)) continue;
        c.SrcAddr = r[i].sad; c.TimerReload = r[i].tmr; c.LoopPos = r[i].pnt; c.Length = r[i].len;
        c.SetCnt(r[i].cnt & 0x7FFFFFFF); c.SetCnt(r[i].cnt);
    }
}

static std::vector<u8> takeComponent(NDS& nds, const std::string& c)
{
    if (c == "sndrestart") return takeChannels(nds);
    { std::vector<u8> b; if (fields3d(nds, c, b, true)) return b; }
    Savestate f(4 << 20);
    doComponent(nds, c, &f);
    f.Finish();
    const u8* b = (const u8*)f.Buffer();
    return std::vector<u8>(b, b + f.Length());
}
static void putComponent(NDS& nds, const std::string& c, std::vector<u8>& data)
{
    if (c == "sndrestart") return; // at the resume, below
    if (fields3d(nds, c, data, false)) return;
    Savestate f(data.data(), (u32)data.size(), false);
    doComponent(nds, c, &f);
}

struct Driver
{
    enum Step { WAIT_START, OPEN1, NAV1, RUN_REF, IDLE1, BACK, GAP, OPEN2, NAV2, RUN_CMP, IDLE2, DONE, FAILED };
    Step step = WAIT_START;
    int saveFrame = 600, gapFrames = 300;
    std::string outDir = ".";
    u32 infoAddr = 0;
    int stepFrame = 0;          // frames in this step
    int inMenuRun = 0, idleRun = 0;
    std::vector<std::pair<u32, int>> taps; // keys, frames (0 keys: a pause)
    int tapLeft = 0;
    std::map<u32, Frame> ref, cmp;  // by testFrames at the end of the frame
    std::string failure;
    std::set<std::string> inject;
    std::map<std::string, std::vector<u8>> captured;
    std::vector<s16> audio; // this frame's output, stereo
    u32 logSector = 0, logSectorCount = 0;

    // RTS_SCRAMBLE=2d,3d: before the load, put garbage in the registers that can't be read back,
    // as if the load came from another scene; whatever the load doesn't restore then shows
    void scramble(NDS& nds)
    {
        const char* e = getenv("RTS_SCRAMBLE");
        if (!e) return;
        std::string v = e;
        u32 seed = 0x1234567;
        auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return (u16)(seed >> 12); };
        auto has = [&](const char* g) { return v.find("2d") != std::string::npos || v.find(g) != std::string::npos; };
        // "flagged": only the registers the census's shadow saw the game write. The others
        // the game hasn't touched since the census came in, so they're the same in every
        // scene of a session and a real load finds them as they were.
        u32 shadow = 0;
        if (v.find("flagged") != std::string::npos)
            for (u32 a = 0x02000000; a < 0x02400000 && !shadow; a += 4)
                if (nds.ARM9Read32(a) == 0x31534E43 && (a & 0x1F) == 0x08 && nds.ARM9Read32(a - 0x108 + 0x1000) == 0x31444853) shadow = a - 0x108 + 0x1000;
        bool written = v.find("written") != std::string::npos;
        if (written) ioprof::trackPaused = true;
        auto flagged = [&](u32 addr) {
            if (written) { int i = ioprof::shIndex(addr); return i < 0 || ioprof::hChanged[i] || ioprof::hChanged[i ^ 1]; }
            if (!shadow) return true;
            u32 o = addr & 0x1FFF, i;
            if (o >= 0x10 && o < 0x58) i = o - 0x10; else if (o >= 0x1010 && o < 0x1058) i = o - 0x1010 + 0x48; else if (o >= 0x330 && o < 0x3C0) i = o - 0x330 + 0x90; else return true;
            return nds.ARM9Read8(shadow + 0x128 + i) != 0;
        };
        struct W { NDS& n; std::function<bool(u32)> f; void Write16(u32 a, u16 x) { if (f(a)) n.ARM9Write16(a, x); } void Write32(u32 a, u32 x) { if (f(a)) n.ARM9Write32(a, x); } void Write8(u32 a, u8 x) { if (f(a)) n.ARM9Write8(a, x); } } w{nds, flagged};
        for (u32 base : {0x04000000u, 0x04001000u})
        {
            if (has("scroll")) for (u32 a = 0x10; a < 0x20; a += 2) w.Write16(base + a, rnd() & 0x1FF);
            if (has("affine")) for (u32 a = 0x20; a < 0x40; a += 2) w.Write16(base + a, rnd());
            if (has("window")) for (u32 a = 0x40; a < 0x48; a += 2) w.Write16(base + a, rnd());
            if (has("mosaic")) w.Write16(base + 0x4C, rnd());
            if (has("blend")) { w.Write16(base + 0x52, rnd() & 0x1F1F); w.Write16(base + 0x54, rnd() & 0x1F); }
        }
        if (v.find("3d") != std::string::npos)
        {
            for (u32 a = 0x330; a < 0x340; a += 2) w.Write16(0x04000000 + a, rnd());           // edge colors
            w.Write16(0x04000340, rnd() & 0x1F);                                               // alpha test
            w.Write32(0x04000350, ((u32)rnd() << 16) | rnd());                                 // clear color
            w.Write16(0x04000354, rnd());                                                      // clear depth
            w.Write32(0x04000358, rnd() & 0x7FFF); w.Write16(0x0400035C, rnd());         // fog
            for (u32 a = 0x360; a < 0x380; a++) w.Write8(0x04000000 + a, rnd() & 0x7F);        // fog table
            for (u32 a = 0x380; a < 0x3C0; a += 2) w.Write16(0x04000000 + a, rnd());           // toon table
        }
        if (written) { ioprof::trackPaused = false; int n = 0; for (int i = 0; i < 0x120; i++) n += ioprof::hChanged[i]; fprintf(stderr, "rts-scramble: %s (%d bytes the game changed)\n", e, n); } else
        fprintf(stderr, "rts-scramble: %s%s\n", e, shadow ? " (flagged only)" : "");
    }
    u32 rd(NDS& nds, u32 off) { return nds.ARM9Read32(infoAddr + off); }

    bool findInfo(NDS& nds)
    {
        // the whole 16 MB in dsi mode (the block is at 0x02FFF680 there)
        u32 end = nds.ConsoleType == 1 ? 0x03000000 : 0x02400000;
        for (u32 a = 0x02000000; a < end; a += 4)
            if (nds.ARM9Read32(a) == RTS_INFO_MAGIC && nds.ARM9Read32(a + 4) == RTS_BASE)
            { infoAddr = a; fprintf(stderr, "rts-test: rts_info at %08X\n", a); return true; }
        return false;
    }
    // rts_info_t: menuState +0x18, testFrames +0x38, testMode +0x3C, logSector +0x48
    bool menuBusy(NDS& nds)
    {
        u32 m = rd(nds, 0x18);
        return m == 0x44524453 || m == 0x52574453 || (m & 0xFFFF) == 0x3741;
    }
    // the menu runs while the block's request word is EXIT_REQUEST_PARK_MAGIC + 2 (RUN)
    // ... and on until the arm7 has put the game's memory back and answered CANCEL
    // (FREEZE_COMBO in between: the menu has answered, the arm9 waits)
    bool inMenu(NDS& nds) { u32 m = infoAddr ? nds.ARM9Read32(infoAddr - 0x110 + 0x0C) : 0; return m == 0x4B524152 || m == 0x435A5246; }
    bool menuUp(NDS& nds) { return infoAddr && nds.ARM9Read32(infoAddr - 0x110 + 0x0C) == 0x4B524152; }
    int upRun = 0;

    void tap(u32 keys, int n = 1)
    {
        for (int i = 0; i < n; i++) { taps.push_back({keys, 4}); taps.push_back({0, 14}); }
    }
    u32 tapKeys()
    {
        while (!taps.empty())
        {
            if (tapLeft == 0) tapLeft = taps.front().second;
            u32 k = taps.front().first;
            if (--tapLeft == 0) taps.erase(taps.begin());
            return k;
        }
        return 0;
    }
    int curFrame = 0;
    void go(Step s) { static const char* n[] = {"wait","open1","nav1","run_ref","idle1","back","gap","open2","nav2","run_cmp","idle2","done","failed"}; fprintf(stderr, "rts-step %s at frame %d\n", n[s], curFrame); step = s; stepFrame = 0; idleRun = 0; }
    void fail(const std::string& why) { failure = why; fprintf(stderr, "rts-test: %s\n", why.c_str()); go(FAILED); }

    // the keys for this frame; 0xFFFFFFFF lets the normal key script through
    u32 keys(NDS& nds, int frame)
    {
        stepFrame++; curFrame = frame;
        switch (step)
        {
            case WAIT_START: if (frame >= saveFrame) { go(OPEN1); if (!infoAddr && !findInfo(nds)) fail("no rts_info in main memory (is the exit combo in?)"); } return ~0u;
            case OPEN1: case OPEN2:
                if (upRun >= 10 && step == OPEN2) scramble(nds);
                if (upRun >= 10) { taps.push_back({0, 30}); /* the menu may still be setting up (pokemon's takes ~50 frames) */ tap(K_DOWN, step == OPEN1 ? 3 : 4); tap(K_A); if (step == OPEN2) { taps.push_back({0, 60}); tap(K_A); } go(step == OPEN1 ? NAV1 : NAV2); return 0; }
                if (stepFrame > 600) { fail("the menu didn't open"); return 0; }
                return K_L | K_R | K_UP | K_SELECT;
            case BACK:
                if (taps.empty() && !inMenuRun) { go(GAP); }
                return tapKeys();
            case GAP: if (stepFrame >= gapFrames) go(OPEN2); return ~0u;
            case RUN_REF: case RUN_CMP:
                if (taps.empty() && !inMenu(nds))
                {
                    // RTS_AFTER=tf:KEYS:len,... - the same presses after both resumes, by test frame
                    u32 tf = rd(nds, 0x38), k = 0;
                    // after the load, press on the same game frame as the reference: line up the
                    // frames recorded so far (as the report does) and shift the presses by that
                    int shift = 0;
                    if (step == RUN_CMP && !cmp.empty())
                    {
                        int best = -1;
                        for (int o : {0, 1, -1, 2, -2, 3, -3})
                        {
                            int m = 0;
                            for (auto& [t, fr] : cmp) { auto r = ref.find(t + o); if (r != ref.end() && r->second.hash == fr.hash) m++; }
                            if (m > best) { best = m; shift = o; }
                        }
                    }
                    if (step == RUN_CMP && getenv("RTS_AFTER")) { static int lastShift = 99; if (shift != lastShift) fprintf(stderr, "rts-after: shift %d at test frame %u\n", shift, tf); lastShift = shift; }
                    tf += shift;
                    if (const char* a = getenv("RTS_AFTER"))
                    {
                        std::string v = a; size_t q = 0;
                        while (q < v.size())
                        {
                            size_t e = v.find(',', q); if (e == std::string::npos) e = v.size();
                            int f0, l; char kn[64];
                            if (sscanf(v.substr(q, e - q).c_str(), "%d:%63[^:]:%d", &f0, kn, &l) == 3 && (int)(TEST_FRAMES - tf) >= f0 && (int)(TEST_FRAMES - tf) < f0 + l) k |= parseKeys(kn);
                            q = e + 1;
                        }
                    }
                    return k;
                }
                return tapKeys();
            default: return tapKeys();
        }
    }

    void record(std::map<u32, Frame>& into, NDS& nds, u32 tf)
    {
        Frame f;
        void* top = nullptr; void* bot = nullptr;
        nds.GPU.GetFramebuffers(&top, &bot);
        f.pixels.resize(256 * 384);
        if (top && bot)
        {
            memcpy(f.pixels.data(), top, 256 * 192 * 4);
            memcpy(f.pixels.data() + 256 * 192, bot, 256 * 192 * 4);
        }
        u32 h = 2166136261u;
        for (u32 p : f.pixels) h = (h ^ (p & 0xFFFFFF)) * 16777619u;
        f.hash = h;
        f.snap = snapshot(nds);
        f.audio = audio;
        into[tf] = std::move(f);
    }

    // after each frame
    void after(NDS& nds, int frame)
    {
        bool menu = inMenu(nds);
        inMenuRun = menu ? inMenuRun + 1 : 0;
        upRun = menuUp(nds) ? upRun + 1 : 0;
        if (step == WAIT_START || step == FAILED || step == DONE) return;
        if (!infoAddr && !findInfo(nds)) { fail("no rts_info in main memory (is the exit combo in?)"); return; }
        u32 tf = rd(nds, 0x38), mode = rd(nds, 0x3C);
        if (const char* w = getenv("RTS_WATCH"))
        {
            static u32 last = 0xDEADBEEF; u32 addr = strtoul(w, nullptr, 16);
            u32 v = nds.ARM9Read32(addr);
            if (v != last && (step == NAV2 || step == RUN_CMP || step == NAV1 || step == RUN_REF))
                fprintf(stderr, "watch frame %d step %d menu %d menuState %08X combo %08X: [%08X] = %08X\n", frame, step, menu, rd(nds, 0x18), nds.ARM9Read32(infoAddr - 0x110 + 0x0C), addr, v);
            last = v;
        }
        if (step == NAV1 && menu && !taps.empty() == false)
            for (auto& c : inject) captured[c] = takeComponent(nds, c);
        if (step == NAV2 && menu && taps.empty())
            for (auto& c : inject) if (captured.count(c)) putComponent(nds, c, captured[c]);
        switch (step)
        {
            case NAV1: case NAV2:
                if (taps.empty() && !menu) go(step == NAV1 ? RUN_REF : RUN_CMP);
                else if (stepFrame > 20000) { char m[96]; snprintf(m, sizeof m, "the game didn't resume after the menu (combo %08X, menuState %08X)", nds.ARM9Read32(infoAddr - 0x110 + 0x0C), rd(nds, 0x18)); fail(m); }
                if (step != RUN_REF && step != RUN_CMP) break;
                [[fallthrough]];
            case RUN_REF: case RUN_CMP:
                if (!menu && mode && tf >= 1 && tf <= TEST_FRAMES)
                {
                    auto& into = step == RUN_REF ? ref : cmp;
                    if (step == RUN_CMP && into.empty() && inject.count("sndrestart") && captured.count("sndrestart"))
                        restartChannels(nds, captured["sndrestart"]);
                    record(into, nds, tf);
                    if (TEST_FRAMES - tf < 6)
                    {
                        auto& ram = into[tf].ram;
                        ram.resize(0x400000 + 0x10000 + 0x8000);
                        memcpy(ram.data(), nds.MainRAM, 0x400000);
                        memcpy(ram.data() + 0x400000, nds.ARM7WRAM, 0x10000);
                        memcpy(ram.data() + 0x410000, nds.SharedWRAM, 0x8000);
                    }
                }
                if (inMenuRun >= 10) go(step == RUN_REF ? IDLE1 : IDLE2);
                else if (stepFrame > 2000) fail("the test freeze didn't come");
                break;
            case IDLE1: case IDLE2:
                idleRun = (menu && !menuBusy(nds)) ? idleRun + 1 : 0;
                if (idleRun >= 240)
                {
                    if (step == IDLE1) { tap(K_B); go(BACK); }
                    else { logSector = rd(nds, 0x48); logSectorCount = rd(nds, 0x4C); go(DONE); fprintf(stderr, "rts-test: done at frame %d\n", frame); }
                }
                else if (stepFrame > 60000) fail("the menu stayed busy");
                break;
            default: break;
        }
    }
};

static void writeSideBySide(const std::string& path, const Frame& a, const Frame& b)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "P6\n768 384\n255\n");
    for (int y = 0; y < 384; y++)
        for (int x = 0; x < 768; x++)
        {
            int col = x / 256, px = x % 256;
            u32 c;
            if (col == 0) c = a.pixels[y * 256 + px];
            else if (col == 1) c = b.pixels[y * 256 + px];
            else c = ((a.pixels[y * 256 + px] ^ b.pixels[y * 256 + px]) & 0xFFFFFF) ? 0xFF00FF : (a.pixels[y * 256 + px] >> 2) & 0x3F3F3F;
            u8 rgb[3] = { (u8)(c >> 16), (u8)(c >> 8), (u8)c };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

// The comparison, as text. Returns 0 when everything matched.
static int report(Driver& d, FILE* out)
{
    if (d.step != Driver::DONE) { fprintf(out, "result: FAILED (%s)\n", d.failure.c_str()); return 2; }
    int frames = 0, screenDiff = 0, firstScreen = -1;
    std::map<std::string, std::pair<int, std::pair<u64, u64>>> fields; // name -> frames differing, first ref/cmp
    std::map<std::string, int> firstFrame;
    // The two runs may be a frame apart (where in the frame each resumed): take
    // the offset that lines most screens up, the smallest of equals.
    int offset = 0, bestMatch = -1;
    for (int o : {0, 1, -1, 2, -2, 3, -3})
    {
        int match = 0;
        for (u32 tf = TEST_FRAMES; tf >= 1; tf--)
        {
            auto a = d.ref.find(tf), b = d.cmp.find(tf - o);
            if (a != d.ref.end() && b != d.cmp.end() && a->second.hash == b->second.hash) match++;
        }
        if (match > bestMatch) { bestMatch = match; offset = o; }
    }
    fprintf(out, "after load is %d frame(s) %s the reference%s\n", abs(offset), offset >= 0 ? "behind" : "ahead", offset ? " (lined up for the comparison)" : "");
    const Frame* ramA = nullptr; const Frame* ramB = nullptr;
    int worstPx = 0, worstK = -1; std::string diffList;
    for (u32 tf = TEST_FRAMES; tf >= 1; tf--)
    {
        auto a = d.ref.find(tf), b = d.cmp.find(tf - offset);
        if (a != d.ref.end() && b != d.cmp.end() && !ramA && !a->second.ram.empty() && !b->second.ram.empty() && TEST_FRAMES - tf >= 2)
        { ramA = &a->second; ramB = &b->second; }
        if (a == d.ref.end() || b == d.cmp.end()) continue;
        frames++;
        int k = TEST_FRAMES - tf;
        if (a->second.hash != b->second.hash)
        {
            if (firstScreen < 0) { firstScreen = k; writeSideBySide(d.outDir + "/first_diff.ppm", a->second, b->second); }
            screenDiff++;
            int px = 0; for (size_t i = 0; i < a->second.pixels.size(); i++) px += (a->second.pixels[i] & 0xFFFFFF) != (b->second.pixels[i] & 0xFFFFFF);
            if (px > worstPx) { worstPx = px; worstK = k; writeSideBySide(d.outDir + "/worst_diff.ppm", a->second, b->second); }
            diffList += " " + std::to_string(k);
            if (getenv("RTS_ALL") && k % 4 == 0) writeSideBySide(d.outDir + "/diff_" + std::to_string(k) + ".ppm", a->second, b->second);
        }
        const Snap& sa = a->second.snap; const Snap& sb = b->second.snap;
        for (size_t i = 0; i < sa.size() && i < sb.size(); i++)
            if (sa[i].second != sb[i].second)
            {
                auto& e = fields[sa[i].first];
                if (e.first++ == 0) { e.second = {sa[i].second, sb[i].second}; firstFrame[sa[i].first] = k; }
            }
    }
    if (worstK >= 0) fprintf(out, "worst screen: frame %d, %d pixels (worst_diff.ppm); differing frames:%s\n", worstK, worstPx, diffList.substr(0, 400).c_str());
    fprintf(out, "frames compared: %d (reference %zu, after load %zu)\n", frames, d.ref.size(), d.cmp.size());
    fprintf(out, "screens differing: %d", screenDiff);
    if (firstScreen >= 0) fprintf(out, ", first at frame %d after the resume (first_diff.ppm: reference | after load | differences)", firstScreen);
    fprintf(out, "\nregisters differing: %zu\n", fields.size());
    for (auto& [name, e] : fields)
        fprintf(out, "  %-28s %3d frames, first at %3d: reference %llX, after load %llX\n", name.c_str(), e.first,
            firstFrame[name], (unsigned long long)e.second.first, (unsigned long long)e.second.second);
    if (ramA)
    {
        const std::vector<u8>& refRam = ramA->ram; const std::vector<u8>& cmpRam = ramB->ram;
        // differing 32-byte lines, merged into ranges
        fprintf(out, "memory differing two frames after the resume (lined up):\n");
        u32 total = 0, shown = 0;
        for (u32 i = 0; i < refRam.size(); )
        {
            if (refRam[i] == cmpRam[i]) { i++; continue; }
            u32 start = i, last = i;
            u32 n = 0;
            while (i < refRam.size() && i - last < 64) { if (refRam[i] != cmpRam[i]) { last = i; n++; } i++; }
            total += n;
            u32 addr = start < 0x400000 ? 0x02000000 + start : start < 0x410000 ? 0x03800000 + start - 0x400000 : 0x03000000 + start - 0x410000;
            if (shown++ < 40)
            {
                fprintf(out, "  %08X-%08X %5u bytes:", addr, addr + (last - start), n);
                for (u32 k = start; k <= last && k < start + 8; k++) fprintf(out, " %02X>%02X", refRam[k], cmpRam[k]);
                fprintf(out, "\n");
            }
        }
        fprintf(out, "  %u bytes in %u ranges\n", total, shown);
        { FILE* t = fopen((d.outDir + "/regs_ref.txt").c_str(), "w"); if (t) { for (auto& [n, v] : ramA->snap) fprintf(t, "%s %llX\n", n.c_str(), (unsigned long long)v); fclose(t); } }
        FILE* r = fopen((d.outDir + "/ram_ref.bin").c_str(), "wb"); if (r) { fwrite(refRam.data(), 1, refRam.size(), r); fclose(r); }
        r = fopen((d.outDir + "/ram_cmp.bin").c_str(), "wb"); if (r) { fwrite(cmpRam.data(), 1, cmpRam.size(), r); fclose(r); }
    }
    {
        // sound over the lined-up frames
        std::vector<s16> ra, ca;
        for (u32 tf = TEST_FRAMES; tf >= 1; tf--)
        {
            auto a = d.ref.find(tf), b = d.cmp.find(tf - offset);
            if (a == d.ref.end() || b == d.cmp.end()) continue;
            size_t n = std::min(a->second.audio.size(), b->second.audio.size());
            ra.insert(ra.end(), a->second.audio.begin(), a->second.audio.begin() + n);
            ca.insert(ca.end(), b->second.audio.begin(), b->second.audio.begin() + n);
        }
        double er = 0, ec = 0, ed = 0;
        for (size_t i = 0; i < ra.size(); i++) { er += (double)ra[i] * ra[i]; ec += (double)ca[i] * ca[i]; ed += (double)(ra[i] - ca[i]) * (ra[i] - ca[i]); }
        double n = ra.size() ? (double)ra.size() : 1;
        double rr = sqrt(er / n), rc = sqrt(ec / n), rd = sqrt(ed / n);
        fprintf(out, "sound: level reference %.0f, after load %.0f; difference %.0f (%.0f%% of the reference)\n", rr, rc, rd, rr > 0 ? 100 * rd / rr : 0.0);
        auto wav = [&](const std::string& path, const std::vector<s16>& v)
        {
            FILE* w = fopen(path.c_str(), "wb"); if (!w) return;
            u32 bytes = (u32)v.size() * 2, rate = 32768, br = rate * 4, sz = 36 + bytes; u16 fmt = 1, ch = 2, al = 4, bits = 16; u32 sixteen = 16;
            fwrite("RIFF", 1, 4, w); fwrite(&sz, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w); fwrite(&sixteen, 4, 1, w);
            fwrite(&fmt, 2, 1, w); fwrite(&ch, 2, 1, w); fwrite(&rate, 4, 1, w); fwrite(&br, 4, 1, w); fwrite(&al, 2, 1, w); fwrite(&bits, 2, 1, w);
            fwrite("data", 1, 4, w); fwrite(&bytes, 4, 1, w); fwrite(v.data(), 2, v.size(), w); fclose(w);
        };
        wav(d.outDir + "/sound_ref.wav", ra);
        wav(d.outDir + "/sound_load.wav", ca);
    }
    int bad = frames == 0 || screenDiff || !fields.empty();
    fprintf(out, "result: %s\n", frames == 0 ? "NO FRAMES" : bad ? "DIFFERS" : "MATCH");
    return bad;
}

}
