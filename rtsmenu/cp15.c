// The menu's cp15 access, in arm code: the rest is thumb, for room.

#include "rtsmenu.h"
#include "stateint.h"

u32 cp15Read(int which)
{
    u32 v = 0;
    switch (which)
    {
        case 0: asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(v)); break;
        case 1: asm volatile("mrc p15, 0, %0, c9, c1, 0" : "=r"(v)); break;
        case 2: asm volatile("mrc p15, 0, %0, c9, c1, 1" : "=r"(v)); break;
        case 3: asm volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(v)); break;
        case 4: asm volatile("mrc p15, 0, %0, c2, c0, 1" : "=r"(v)); break;
        case 5: asm volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(v)); break;
        case 6: asm volatile("mrc p15, 0, %0, c5, c0, 2" : "=r"(v)); break;
        case 7: asm volatile("mrc p15, 0, %0, c5, c0, 3" : "=r"(v)); break;
        case 8: asm volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(v)); break;
        case 9: asm volatile("mrc p15, 0, %0, c6, c1, 0" : "=r"(v)); break;
        case 10: asm volatile("mrc p15, 0, %0, c6, c2, 0" : "=r"(v)); break;
        case 11: asm volatile("mrc p15, 0, %0, c6, c3, 0" : "=r"(v)); break;
        case 12: asm volatile("mrc p15, 0, %0, c6, c4, 0" : "=r"(v)); break;
        case 13: asm volatile("mrc p15, 0, %0, c6, c5, 0" : "=r"(v)); break;
        case 14: asm volatile("mrc p15, 0, %0, c6, c6, 0" : "=r"(v)); break;
        case 15: asm volatile("mrc p15, 0, %0, c6, c7, 0" : "=r"(v)); break;
    }
    return v;
}

// the data permissions (c5, c0, 2)
void cp15WritePermissions(u32 v)
{
    asm volatile("mcr p15, 0, %0, c5, c0, 2" :: "r"(v) : "memory");
}

void dcFlushAll(void)
{
    // clean and invalidate by set/way: 4 ways of 32 lines of 32 bytes
    for (u32 way = 0; way < 4; way++)
        for (u32 line = 0; line < 32; line++)
            asm volatile("mcr p15, 0, %0, c7, c14, 2" :: "r"((way << 30) | (line << 5)) : "memory");
    asm volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

void icInvalidateAll(void)
{
    asm volatile("mcr p15, 0, %0, c7, c5, 0" :: "r"(0) : "memory");
}

void dcInvalidateLine(const volatile void* p)
{
    asm volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(p) : "memory");
}
