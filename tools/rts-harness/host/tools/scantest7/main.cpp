#include "ShadowScan.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace shadow_scan;
// scantest7 ROM: the arm7 binary's sites for the sound channel / timer reload / capture shadow
int main(int argc, char** argv)
{
    FILE* f = fopen(argv[1], "rb"); if (!f) return 1;
    uint8_t h[0x200]; fread(h, 1, 0x200, f);
    u32 off, entry, load, size; memcpy(&off, h + 0x30, 4); memcpy(&entry, h + 0x34, 4); memcpy(&load, h + 0x38, 4); memcpy(&size, h + 0x3C, 4);
    std::vector<uint8_t> b(size); fseek(f, off, SEEK_SET); fread(b.data(), 1, size, f); fclose(f);
    gTargetArm7 = 1;
    Block blk[1] = {{b.data(), load, size}};
    static u32 sites[4096];
    Scanner s(blk, 1, sites, 4096);
    static CopySite copies[512];
    s.SetCopyOutput(copies, 512);
    s.Scan(0, 1);
    fprintf(stderr, "arm7 at %08X size %X entry %08X: %u sites, %u copies, overflow %u\n", load, size, entry, s.SiteCount(), s.CopyCount(), s.Overflow());
    for (u32 i = 0; i < s.CopyCount(); i++) fprintf(stderr, "copy at %08X -> %08X link %d kind %d\n", copies[i].at, copies[i].callee, copies[i].link, copies[i].kind);
    // the module params through crt0's literal: autoload list start/end, autoload start
    u32 ls = 0, le = 0, as = 0;
    for (u32 o = 0; o + 16 <= 0x400 && o + 16 <= size; o += 4)
    {
        u32 w[4]; memcpy(w, b.data() + o, 16);
        if ((w[0] & 0xFFFFF000) == 0xE59F0000 && w[1] == 0xE5901000 && w[2] == 0xE5902004 && w[3] == 0xE5903008)
        {
            u32 lit; memcpy(&lit, b.data() + o + 8 + (w[0] & 0xFFF), 4);
            memcpy(&ls, b.data() + lit - load, 4); memcpy(&le, b.data() + lit - load + 4, 4); memcpy(&as, b.data() + lit - load + 8, 4);
            break;
        }
    }
    fprintf(stderr, "autoload list %08X-%08X from %08X\n", ls, le, as);
    auto adjust = [&](u32 a) {
        u32 cur = as;
        for (u32 e = ls; e && e < le; e += 12)
        {
            u32 tgt, sz; memcpy(&tgt, b.data() + e - load, 4); memcpy(&sz, b.data() + e - load + 4, 4);
            if (a >= cur && a < cur + sz) return a - cur + tgt;
            cur += sz;
        }
        return a;
    };
    for (u32 i = 0; i < s.SiteCount(); i++) { u32 a = sites[i] & ~1u; u32 op = 0; memcpy(&op, b.data() + (a - load), (sites[i] & 1) ? 2 : 4); printf("%08X %08X %08X\n", adjust(a) | (sites[i] & 1), op, sites[i]); }
}
