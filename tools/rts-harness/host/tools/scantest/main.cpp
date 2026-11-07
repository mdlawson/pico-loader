#include "ShadowScan.h"
#include <cstdio>
#include <vector>
using namespace shadow_scan;
static std::vector<uint8_t> rd(const char* p) { FILE* f = fopen(p, "rb"); std::vector<uint8_t> v; if (!f) return v; fseek(f, 0, SEEK_END); v.resize(ftell(f)); fseek(f, 0, SEEK_SET); fread(v.data(), 1, v.size(), f); fclose(f); return v; }
int main(int argc, char** argv)
{
    auto ram = rd(argv[1]), itcm = rd(argv[2]);
    Block b[2] = {{itcm.data(), 0x01FF8000, (u32)itcm.size()}, {ram.data(), 0x02000000, (u32)ram.size()}};
    static u32 sites[4096];
    Scanner s(b, 2, sites, 4096);
    static CopySite copies[512];
    s.SetCopyOutput(copies, 512);
    s.Scan(0, 2);
    for (u32 i = 0; i < s.CopyCount(); i++) fprintf(stderr, "copy at %08X -> %08X link %d kind %d\n", copies[i].at, copies[i].callee, copies[i].link, copies[i].kind);
    for (u32 i = 0; i < s.SiteCount(); i++) printf("%08X\n", sites[i]);
    fprintf(stderr, "%u sites, overflow %u\n", s.SiteCount(), s.Overflow());
}
