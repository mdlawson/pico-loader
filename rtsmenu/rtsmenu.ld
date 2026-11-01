/* The rts menu at RTS_BASE (common/sharedMemory.h). Of the RTS_STASH_SIZE bytes
   the arm7 frees there: the image, its bss and stack in the first 0x10000, the
   copy of vram bank A in the 0x20000 after. */
ENTRY(_start)
MEMORY { stash (rwx) : ORIGIN = 0x02100000, LENGTH = 0x30000 }
SECTIONS
{
    .text : { KEEP(*(.text.start)) *(.text .text.*) *(.rodata .rodata.*) *(.data .data.*) . = ALIGN(4); } > stash
    .bss (NOLOAD) : { __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(4); __bss_end = .; } > stash
    __stack_top = ORIGIN(stash) + 0x10000;
    __vram_copy = ORIGIN(stash) + 0x10000;
    ASSERT(__bss_end <= ORIGIN(stash) + 0x8000, "rts menu too big: keep 0x8000 for the stack")
    /DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}
