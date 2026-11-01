/* The rts menu at RTS_BASE (common/sharedMemory.h). Of the RTS_STASH_SIZE bytes
   the arm7 frees there: the image and its bss in the first 0x4000, then 0x8000
   of scratch the arm7 reads and writes, the stack up to 0x10000, and the copy of
   vram bank A in the 0x20000 after. */
ENTRY(_start)
MEMORY { stash (rwx) : ORIGIN = 0x02100000, LENGTH = 0x30000 }
SECTIONS
{
    .text : { KEEP(*(.text.start)) *(.text .text.*) *(.rodata .rodata.*) *(.data .data.*) . = ALIGN(4); } > stash
    .bss (NOLOAD) : { __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(4); __bss_end = .; } > stash
    __stack_top = ORIGIN(stash) + 0x10000;
    __vram_copy = ORIGIN(stash) + 0x10000;
    __scratch = ORIGIN(stash) + 0x4000;
    ASSERT(__bss_end <= __scratch, "rts menu too big: it has to end below the scratch area")
    /DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}
