#pragma once
// A static scan of ARM code for the stores that write the display registers no
// one can read back (2D scroll, affine, windows, mosaic, blend of both engines;
// the 3D settings at 0x04000330-0x040003BF). Plain C++ with no DS dependencies,
// so the same code runs on the host against memory dumps.
//
// From each place the code puts an IO base in a register (a literal pool word
// loaded pc-relative, or mov rX,#0x04000000), the scan follows that register
// forward through simple arithmetic, conditional branches (their fall-through)
// and calls (into the callee, with the registers it gets), until an
// unconditional branch, a return, or the register's redefinition. Each store
// it can work out to land on a shadowed register is a site.
#include <stdint.h>

namespace shadow_scan
{

typedef uint32_t u32;
typedef uint16_t u16;

/// @brief The code to scan: one block of memory and the address it runs at.
struct Block
{
    const uint8_t* data;
    u32 runAddress; // where data[0] runs
    u32 size;
};

/// @brief Whether the shadow keeps the register at io address a.
static inline bool IsShadowed(u32 a)
{
    if (a >= 0x04001000 && a < 0x04002000)
    {
        u32 o = a - 0x04001000;
        return o >= 0x10 && o < 0x58;
    }
    u32 o = a - 0x04000000;
    return (o >= 0x10 && o < 0x58) || (o >= 0x330 && o < 0x3C0);
}

class Scanner
{
public:
    /// @param blocks The code, in blocks that may call each other.
    /// @param sites Where to put the sites found (run addresses; bit 0 set for thumb).
    Scanner(const Block* blocks, u32 blockCount, u32* sites, u32 maxSites)
        : _blocks(blocks), _blockCount(blockCount), _sites(sites), _maxSites(maxSites) { }

    /// @brief Scans the blocks [first, first + count) for io bases; calls may go into any block.
    void Scan(u32 first, u32 count);

    u32 SiteCount() const { return _siteCount; }
    u32 Overflow() const { return _overflow; }

private:
    enum : uint8_t { NONE = 0, CONST = 1, IO = 2 };
    struct Regs { uint8_t kind[16]; u32 val[16]; };

    const Block* _blocks;
    u32 _blockCount;
    u32* _sites;
    u32 _maxSites;
    u32 _siteCount = 0;
    u32 _overflow = 0;
    // calls already followed: callee and the io arguments (a small ring)
    u32 _callKey[64][2];
    u32 _callKeyN = 0;

    const Block* BlockOf(u32 a) const;
    bool Read32(u32 a, u32& v) const;
    void AddSite(u32 site);
    bool AnyIo(const Regs& r) const;
    void Trace(u32 pc, Regs regs, int depth);
    void TraceThumb(u32 pc, Regs regs, int depth);
    bool Read16(u32 a, u32& v) const;
    void Call(u32 target, bool thumb, const Regs& r, int depth);
    bool InLoop(u32 site, bool thumb) const;
    void AddSiteAt(u32 site, bool thumb, int depth);
};

}
