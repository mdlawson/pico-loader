#include "Savestate.h"
// dshost: run melonDS's core headless for Pico Loader tests.
//   dshost --bios9 f [--bios7 f] --cart launcher.nds --sd dir [--sdimg f] --frames N
//          [--key FRAME:KEYS:LEN]... [--shot FRAME:path.ppm]... [--peek FRAME:ADDR:WORDS]...
//          [--dump FRAME:ADDR:LEN:path]... [--status N] [--verbose] [--dspico dldi.bin]
// --bios9i/--bios7i (with --bios9/--bios7/--firmware): run as a DSi, direct boot.
// --dspico: the cart speaks the DSpico's sd commands, with its DLDI driver patched in.
// KEYS: A B SELECT START RIGHT LEFT UP DOWN R L X Y joined with '+'.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <memory>
#include "NDS.h"
#include "DSi.h"
#include "SPI_Firmware.h"
#include "NDSCart.h"
#include "GPU.h"
#include "GPU_Soft.h"
#include "Args.h"
#include "FATStorage.h"

using namespace melonDS;
namespace melonDS::Platform { extern int gFrame; extern int gLogVerbose; extern std::string gSavePath; }
namespace melonDS::NDSCart { extern std::vector<u8> gDSpicoDLDI; extern u32 gDSpicoPollDelay; }

static std::vector<u8> readFile(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) { fprintf(stderr, "can't read %s\n", p.c_str()); exit(1); }
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), {});
}

struct KeyEv { int frame, len; u32 mask; };
struct TouchEv { int frame, x, y, len; };
struct Shot { int frame; std::string path; };
struct Peek { int frame; u32 addr, words; };
struct Dump { int frame; u32 addr, len; std::string path; };

// The arm9's view: its tcms first (the bus doesn't see them), then the bus.
static u32 peek9(melonDS::NDS& nds, u32 a)
{
    auto& c = nds.ARM9;
    if ((a & c.DTCMMask) == c.DTCMBase) return *(u32*)&c.DTCM[a & (melonDS::DTCMPhysicalSize - 4)];
    if (a < c.ITCMSize) return *(u32*)&c.ITCM[a & (melonDS::ITCMPhysicalSize - 4)];
    return nds.ARM9Read32(a);
}

static u32 parseKeys(std::string s)
{
    static const char* names[] = {"A","B","SELECT","START","RIGHT","LEFT","UP","DOWN","R","L","X","Y"};
    u32 m = 0; size_t p = 0;
    while (p <= s.size())
    {
        size_t q = s.find('+', p); if (q == std::string::npos) q = s.size();
        std::string k = s.substr(p, q - p);
        for (int i = 0; i < 12; i++) if (k == names[i]) m |= 1u << i;
        p = q + 1;
    }
    return m;
}
#include "ioprof.h"
#include "rtstest.h"
namespace melonDS { extern u32 gHarnessAbortRing[64][3]; extern u32 gHarnessAbortN; extern u32 gHarness7Ring[4096]; extern u32 gHarness7N; extern int gHarness7Arm; extern u32 gHarness9Ring[4096]; extern u32 gHarness9N; extern int gHarness9Arm; }

static void writePPM(NDS& nds, const std::string& path)
{
    void* top = nullptr; void* bot = nullptr;
    nds.GPU.GetFramebuffers(&top, &bot);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f || !top || !bot) { if (f) fclose(f); return; }
    fprintf(f, "P6\n256 384\n255\n");
    for (int s = 0; s < 2; s++)
    {
        const u32* fb = (const u32*)(s ? bot : top);
        for (int i = 0; i < 256 * 192; i++)
        {
            u32 c = fb[i];
            u8 rgb[3] = { (u8)((c >> 16) & 0xFF), (u8)((c >> 8) & 0xFF), (u8)(c & 0xFF) };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

int main(int argc, char** argv)
{
    std::string bios9i, bios7i, firmware;
    std::string bios9, bios7, cart, sd, sdimg = "sd.img";
    int frames = 600, status = 60;
    bool mpuTrace = false;
    rtstest::Driver rts; bool rtsOn = false;
    u32 lastMpu[12] = {};
    std::vector<TouchEv> touches;
    std::vector<KeyEv> keys; std::vector<Shot> shots; std::vector<Peek> peeks; std::vector<Dump> dumps;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        auto next = [&]{ return std::string(argv[++i]); };
        if (a == "--bios9") bios9 = next();
        else if (a == "--bios7") bios7 = next();
        else if (a == "--bios9i") bios9i = next();
        else if (a == "--bios7i") bios7i = next();
        else if (a == "--firmware") firmware = next();
        else if (a == "--cart") cart = next();
        else if (a == "--sd") sd = next();
        else if (a == "--sdimg") sdimg = next();
        else if (a == "--frames") frames = atoi(next().c_str());
        else if (a == "--status") status = atoi(next().c_str());
        else if (a == "--dspico") NDSCart::gDSpicoDLDI = readFile(next());
        else if (a == "--dspico-delay") NDSCart::gDSpicoPollDelay = atoi(next().c_str());
        else if (a == "--mpu") mpuTrace = true;
        else if (a == "--rts-test") { rtsOn = true; sscanf(next().c_str(), "%d:%d", &rts.saveFrame, &rts.gapFrames); }
        else if (a == "--rts-out") rts.outDir = next();
        else if (a == "--rts-inject") { std::string v = next(); size_t q = 0; while (q <= v.size()) { size_t e = v.find(',', q); if (e == std::string::npos) e = v.size(); rts.inject.insert(v.substr(q, e - q)); q = e + 1; } }
        else if (a == "--touch") { TouchEv t; sscanf(next().c_str(), "%d:%d:%d:%d", &t.frame, &t.x, &t.y, &t.len); touches.push_back(t); }
        else if (a == "--verbose") Platform::gLogVerbose++;
        else if (a == "--key") { std::string s = next(); int f, l; char k[64]; sscanf(s.c_str(), "%d:%63[^:]:%d", &f, k, &l); keys.push_back({f, l, parseKeys(k)}); }
        else if (a == "--shot") { std::string s = next(); size_t c = s.find(':'); shots.push_back({atoi(s.substr(0, c).c_str()), s.substr(c + 1)}); }
        else if (a == "--peek") { std::string s = next(); Peek p; sscanf(s.c_str(), "%d:%x:%u", &p.frame, &p.addr, &p.words); peeks.push_back(p); }
        else if (a == "--dump") { std::string s = next(); Dump d; char path[512]; sscanf(s.c_str(), "%d:%x:%x:%511s", &d.frame, &d.addr, &d.len, path); d.path = path; dumps.push_back(d); }
        else { fprintf(stderr, "unknown argument %s\n", a.c_str()); return 1; }
    }

    std::unique_ptr<NDS> nds;
    auto fillCommon = [&](NDSArgs& args)
    {
        args.JIT = std::nullopt;
        if (!bios9.empty()) { auto b = readFile(bios9); args.ARM9BIOS = std::make_unique<ARM9BIOSImage>(); memcpy(args.ARM9BIOS->data(), b.data(), std::min(b.size(), args.ARM9BIOS->size())); }
        if (!bios7.empty()) { auto b = readFile(bios7); args.ARM7BIOS = std::make_unique<ARM7BIOSImage>(); memcpy(args.ARM7BIOS->data(), b.data(), std::min(b.size(), args.ARM7BIOS->size())); }
        if (!firmware.empty())
        {
            auto b = readFile(firmware);
            Firmware fw(b.data(), b.size());
            // boot straight into the cart, as the dumps this was developed with did: a dump with
            // the boot menu set to manual stops at the DS menu (only the copy in memory changes)
            for (auto& u : fw.GetUserData()) u.Settings |= Firmware::BootMenu::Autostart;
            fw.UpdateChecksums();
            args.Firmware = std::move(fw);
        }
    };
    if (!bios9i.empty())
    {
        // a DSi, booted straight into the cart's dsi binaries (no NAND)
        DSiArgs args;
        fillCommon(args);
        auto b9 = readFile(bios9i); memcpy(args.ARM9iBIOS->data(), b9.data(), std::min(b9.size(), args.ARM9iBIOS->size()));
        auto b7 = readFile(bios7i); memcpy(args.ARM7iBIOS->data(), b7.data(), std::min(b7.size(), args.ARM7iBIOS->size()));
        // the console's own sd slot gets the same files, for the launcher (BlocksDS
        // won't mount the flashcart's card on a DSi without it)
        if (!sd.empty())
            args.DSiSDCard = FATStorage(FATStorageArgs{ sdimg + ".dsisd", (u64)(getenv("DSHOST_SD_MB") ? atoi(getenv("DSHOST_SD_MB")) : 256) << 20, false, sd });
        nds = std::make_unique<DSi>(std::move(args), nullptr);
    }
    else
    {
        NDSArgs args;
        fillCommon(args);
        nds = std::make_unique<NDS>(std::move(args), nullptr);
    }
    nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
    nds->Reset();
    // melonDS starts the RTC with "power lost" set, and some firmware dumps then stop at
    // "you must adjust system settings" instead of booting the cart. Clear just that flag
    // (the clock itself is left as it was).
    {
        RTC::StateData rtc;
        nds->RTC.GetState(rtc);
        rtc.StatusReg1 &= ~0x80;
        nds->RTC.SetState(rtc);
    }

    auto rom = readFile(cart);
    NDSCart::NDSCartArgs cartArgs;
    if (!sd.empty())
        cartArgs.SDCard = FATStorageArgs{ sdimg, (u64)(getenv("DSHOST_SD_MB") ? atoi(getenv("DSHOST_SD_MB")) : 256) << 20, false, getenv("SDIMG_ONLY") ? std::nullopt : std::optional<std::string>(sd) };
    auto c = NDSCart::ParseROM(rom.data(), rom.size(), nullptr, std::move(cartArgs));
    if (!c) { fprintf(stderr, "ParseROM failed\n"); return 1; }
    nds->SetNDSCart(std::move(c));
    if (nds->NeedsDirectBoot()) nds->SetupDirectBoot(cart);
    nds->Start();
    // an emulator snapshot to go on from (the same sd image, untouched since)
    if (const char* ls = getenv("LOAD_STATE"))
    {
        auto st = readFile(ls);
        Savestate in(st.data(), st.size(), false);
        if (st.empty() || !nds->DoSavestate(&in) || in.Error) { fprintf(stderr, "LOAD_STATE %s failed\n", ls); return 1; }
        nds->Start();
    }
    ioprof::start(nds.get());

    for (int f = 0; f < frames; f++)
    {
        u32 pressed = 0;
        for (auto& k : keys) if (f >= k.frame && f < k.frame + k.len) pressed |= k.mask;
        if (rtsOn) { u32 k = rts.keys(*nds, f); if (k != ~0u) pressed = k; }
        nds->SetKeyMask(~pressed & 0xFFF);
        {
            bool touched = false;
            for (auto& t : touches) if (f >= t.frame && f < t.frame + t.len) { nds->TouchScreen(t.x, t.y); touched = true; }
            if (!touched) nds->ReleaseScreen();
        }
        if (const char* a = getenv("PC7_RING"); a && f == atoi(a)) melonDS::gHarness7Arm = 1;
        if (const char* a = getenv("PC9_RING"); a && f == atoi(a)) melonDS::gHarness9Arm = 1;
        melonDS::Platform::gFrame = f; nds->RunFrame();
        ioprof::endFrame(f);
        if (getenv("CENSUS_STATS"))
        {
            static u32 cAddr = 0, lastTraps = 0, lastThumb = 0, lastFrame = 0;
            if (!cAddr && f % 60 == 0)
                for (u32 a = 0x02300000; a < 0x02400000; a += 4)
                    if (nds->ARM9Read32(a + 0x108) == 0x31534E43 && nds->ARM9Read32(a + 0xF4) == a + 0x1000) { cAddr = a; break; }
            if (cAddr && f % 60 == 59)
            {
                u32 traps = nds->ARM9Read32(cAddr + 0x78), thumb = nds->ARM9Read32(cAddr + 0xC4);
                fprintf(stderr, "census f%d: state %u traps/frame %.1f thumb/frame %.1f patched %u fail %u stop %u pc %08X\n", f, nds->ARM9Read32(cAddr + 0x40),
                    (traps - lastTraps) / (double)(f - lastFrame), (thumb - lastThumb) / (double)(f - lastFrame), nds->ARM9Read32(cAddr + 0x110), nds->ARM9Read32(cAddr + 0x114), nds->ARM9Read32(cAddr + 0xB8), nds->ARM9Read32(cAddr + 0xBC));
                lastTraps = traps; lastThumb = thumb; lastFrame = f;
            }
        }
        {
            static std::vector<s16> abuf(8192);
            int n = nds->SPU.ReadOutput(abuf.data(), 2048);
            if (rtsOn) rts.audio.assign(abuf.begin(), abuf.begin() + std::max(n, 0) * 2);
        }
        if (rtsOn)
        {
            rts.after(*nds, f);
            if (rts.step == rtstest::Driver::DONE || rts.step == rtstest::Driver::FAILED) { frames = f + 1; }
        }
        if (mpuTrace)
        {
            u32 now[12];
            for (int r = 0; r < 8; r++) now[r] = nds->ARM9.PU_Region[r];
            now[8] = nds->ARM9.PU_DataRW; now[9] = nds->ARM9.PU_DataCacheable;
            now[10] = nds->ARM9.CP15Control; now[11] = nds->ARM9Read32(0x027FFD9C);
            if (memcmp(now, lastMpu, sizeof(now)))
            {
                fprintf(stderr, "frame %d mpu:", f);
                for (int r = 0; r < 12; r++) fprintf(stderr, " %08X", now[r]);
                fprintf(stderr, "\n");
                memcpy(lastMpu, now, sizeof(now));
            }
        }
        {
            static bool abtShown = false;
            if (!abtShown && (nds->ARM9.CPSR & 0x1F) == 0x17)
            {
                abtShown = true;
                fprintf(stderr, "frame %d arm9 abort: lr_abt %08X spsr_abt %08X sp_abt %08X", f, nds->ARM9.R_ABT[1], nds->ARM9.R_ABT[2], nds->ARM9.R_ABT[0]);
                for (int r = 0; r < 13; r++) fprintf(stderr, " r%d=%08X", r, nds->ARM9.R[r]);
                fprintf(stderr, "\n");
            }
        }
        if (status && f % status == 0)
            fprintf(stderr, "frame %5d  arm9 pc %08X cpsr %08X  arm7 pc %08X  dispcnt %08X\n", f,
                nds->ARM9.R[15], nds->ARM9.CPSR, nds->ARM7.R[15], nds->ARM9Read32(0x04000000));
        for (auto& s : shots) if (s.frame == f) writePPM(*nds, s.path);
        // CHPOS=mask:from:to - the spu channels' play positions each frame
        if (static const char* cp = getenv("CHPOS"); cp)
        {
            static unsigned m = 0, a0 = 0, a1 = 0;
            if (!m) sscanf(cp, "%x:%u:%u", &m, &a0, &a1);
            if ((unsigned)f >= a0 && (unsigned)f <= a1)
            {
                fprintf(stderr, "chpos %d", f);
                for (int i = 0; i < 16; i++) if (m >> i & 1)
                {
                    SPUChannel& c = nds->SPU.HarnessChannel(i);
                    fprintf(stderr, " %d:%c%08X/%d", i, c.Cnt >> 31 ? '+' : '-', c.SrcAddr, c.Pos);
                }
                fprintf(stderr, " vc %u\n", nds->ARM7Read16(0x04000006));
            }
        }
        for (auto& p : peeks) if (p.frame == f)
        {
            fprintf(stderr, "frame %d peek %08X:", f, p.addr);
            for (u32 i = 0; i < p.words; i++) fprintf(stderr, " %08X", p.addr >> 31 ? nds->ARM7Read32((p.addr & 0x7FFFFFFF) + i * 4) : peek9(*nds, p.addr + i * 4)); // bit 31: the arm7's bus
            fprintf(stderr, "\n");
        }
        for (auto& d : dumps) if (d.frame == f)
        {
            FILE* o = fopen(d.path.c_str(), "wb");
            for (u32 i = 0; i < d.len; i += 4) { u32 a = d.addr + i, v; if (a >> 31) v = nds->ARM7Read32((a & 0x7FFFFFFF)); else if (a < 0x02000000) memcpy(&v, nds->ARM9.ITCM + (a & (ITCMPhysicalSize - 1) & ~3u), 4); else v = nds->ARM9Read32(a); fwrite(&v, 4, 1, o); }
            fclose(o);
        }
    }
    fprintf(stderr, "done: arm9 pc %08X arm7 pc %08X\n", nds->ARM9.R[15], nds->ARM7.R[15]);
    nds->Stop();
    int result = 0;
    if (const char* ss = getenv("SAVE_STATE"))
    {
        Savestate out;
        if (nds->DoSavestate(&out) && !out.Error)
        {
            out.Finish();
            FILE* o = fopen(ss, "wb");
            if (o) { fwrite(out.Buffer(), 1, out.Length(), o); fclose(o); }
        }
        else fprintf(stderr, "SAVE_STATE failed\n");
    }
    if (rtsOn)
    {
        std::string rp = rts.outDir + "/report.txt";
        FILE* r = fopen(rp.c_str(), "w");
        result = rtstest::report(rts, r ? r : stdout);
        if (r) fclose(r);
        FILE* e = fopen(rp.c_str(), "r"); char line[512]; while (e && fgets(line, sizeof line, e)) fputs(line, stdout); if (e) fclose(e);
        u32 logSector = rts.logSector, logCount = rts.logSectorCount;
        nds.reset(); // the sd image is written out
        if (logSector && logCount <= 64)
        {
            FILE* img = fopen(sdimg.c_str(), "rb");
            std::vector<char> buf(logCount * 512);
            if (img && !fseek(img, (long)logSector * 512, SEEK_SET) && fread(buf.data(), 1, buf.size(), img) == buf.size())
            {
                FILE* o = fopen((rts.outDir + "/rtslog.txt").c_str(), "wb");
                size_t n = buf.size(); while (n && (buf[n - 1] == 0 || buf[n - 1] == ' ')) n--;
                if (o) { fwrite(buf.data(), 1, n, o); fclose(o); }
            }
            if (img) fclose(img);
        }
        ioprof::report();
        return result;
    }
    if (getenv("IO_DUMP")) { auto& n = *nds; fprintf(stderr, "arm9 IME %X IE %08X IF %08X DISPSTAT %04X | arm7 IME %X IE %08X IF %08X IE2 %08X IF2 %08X DISPSTAT %04X vec7 %08X flags7 %08X | dtcm %08X\n", n.ARM9Read32(0x04000208), n.ARM9Read32(0x04000210), n.ARM9Read32(0x04000214), n.ARM9Read16(0x04000004), n.ARM7Read32(0x04000208), n.ARM7Read32(0x04000210), n.ARM7Read32(0x04000214), n.ARM7Read32(0x04000218), n.ARM7Read32(0x0400021C), n.ARM7Read16(0x04000004), n.ARM7Read32(0x0380FFFC), n.ARM7Read32(0x0380FFF8), n.ARM9.DTCMBase); fprintf(stderr, "scfg9 rom %08X clk %08X ext %08X mc %08X exmem %04X | scfg7 rom %08X clk %08X ext %08X mc %08X exmem %04X | mbk %08X %08X %08X %08X %08X | 9: %08X %08X %08X | 7: %08X %08X %08X | wramcnt %02X\n", n.ARM9Read32(0x04004000), n.ARM9Read32(0x04004004), n.ARM9Read32(0x04004008), n.ARM9Read32(0x04004010), n.ARM9Read16(0x04000204), n.ARM7Read32(0x04004000), n.ARM7Read32(0x04004004), n.ARM7Read32(0x04004008), n.ARM7Read32(0x04004010), n.ARM7Read16(0x04000204), n.ARM9Read32(0x04004040), n.ARM9Read32(0x04004044), n.ARM9Read32(0x04004048), n.ARM9Read32(0x0400404C), n.ARM9Read32(0x04004050), n.ARM9Read32(0x04004054), n.ARM9Read32(0x04004058), n.ARM9Read32(0x0400405C), n.ARM7Read32(0x04004054), n.ARM7Read32(0x04004058), n.ARM7Read32(0x0400405C), n.ARM9Read8(0x04000247)); }
    if (getenv("PC9_RING")) { fprintf(stderr, "pc9 ring (%d):\n", melonDS::gHarness9Arm); for (u32 i = 0; i < 4096; i++) { u32 v = melonDS::gHarness9Ring[(melonDS::gHarness9N + i) & 4095]; if (i >= 4096 - (getenv("PC_RING_N") ? atoi(getenv("PC_RING_N")) : 400)) fprintf(stderr, " %08X", v); } fprintf(stderr, "\n"); }
    if (getenv("PC7_RING")) { fprintf(stderr, "pc7 ring (%d):\n", melonDS::gHarness7Arm); for (u32 i = 0; i < 4096; i++) { u32 v = melonDS::gHarness7Ring[(melonDS::gHarness7N + i) & 4095]; if (i >= 4096 - (getenv("PC_RING_N") ? atoi(getenv("PC_RING_N")) : 400)) fprintf(stderr, " %08X", v); } fprintf(stderr, "\n"); }
    if (getenv("ABORT_RING")) { fprintf(stderr, "aborts: %u\n", melonDS::gHarnessAbortN); for (u32 i = 0; i < 64 && i < melonDS::gHarnessAbortN; i++) { u32* e = melonDS::gHarnessAbortRing[(melonDS::gHarnessAbortN - 64 + i) & 63]; fprintf(stderr, "  abort pc %08X cpsr %08X sp %08X\n", e[0], e[1], e[2]); } }
    ioprof::report();
    return 0;
}
