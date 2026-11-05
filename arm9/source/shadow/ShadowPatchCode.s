.cpu arm946e-s
.syntax unified
.arm

// The register shadow without traps. The loader finds the stores that write the
// display registers no one can read back (ShadowScan.cpp) and puts each one in a
// stub here:
//     <the store, as it was, condition and all>
//     str lr, [sp, #-8]!
//     bl shadow_record
//     .word <the site + 4>
// The site becomes b<its condition> to the stub. The stub's store runs where the
// site's did; the record then works out where it went from the registers after
// it and keeps what it wrote to a shadowed register. A load writes the shadow
// back (rtsmenu/state.c). A stub alone undoes its site: its first word is the
// site's own.
//
// Thumb stores and calls into the sdk's copies and fills to the registers have
// stubs of their own, below.
//
// The space (in the arm7's main memory arena, which the loader opens to the
// arm9 by switching off the sdk's region 2 guard):
//     +0x000 'SHD2', +0x004 version, +0x008 stubs, +0x00C offset of the stubs
//     +0x010 the shadow: 0x120 bytes, 0x04000010-57 at 0, 0x04001010-57 at
//            0x48, 0x04000330-3BF at 0x90
//     +0x130 a byte per shadow byte, 0xFF once the game wrote it
//     +0x250 this code, then the stubs

.equ SHADOW_DATA,   0x10
.equ SHADOW_FLAGS,  0x130

.section "patch_shadow", "ax"
.global patch_shadow_start
patch_shadow_start:
shadow_header:
    .word 0x32444853 // 'SHD2'
    .word 1
    .word 0
    .word 0
shadow_data:
    .space 0x120
shadow_flags:
    .space 0x120

// From a stub: lr = the stub + 12, [sp] = the caller's lr, [sp + 4] free.
.global shadow_record
.type shadow_record, %function
shadow_record:
    stmfd sp!, {r0-r12}
    mrs r11, cpsr
    ldr r10, [lr]
    str r10, [sp, #56] // where to go back to
    ldr r10, [lr, #-12] // the store
    and r9, r10, #0x000F0000
    cmp r9, #0x000E0000
    moveq r9, #0x000D0000 // lr: the stub keeps it just above r12
    ldr r8, [sp, r9, lsr #14] // its base, after the store
    and r9, r10, #0x0E000000
    cmp r9, #0x08000000
    beq record_stm
    tst r10, #0x0C000000
    bne record_str
    // strh / strd: imm8 split in two nibbles
    and r1, r10, #0xF
    and r0, r10, #0xF00
    orr r1, r1, r0, lsr #4
    bl record_address
    and r9, r10, #0x0000F000
    cmp r9, #0x0000E000
    moveq r9, #0x0000D000
    ldr r0, [sp, r9, lsr #10]
    tst r10, #0x40 // sh = 11: strd
    moveq r2, #2
    movne r2, #4
    bl record_bytes
    tst r10, #0x40
    beq record_done
    add r9, r9, #0x1000
    ldr r0, [sp, r9, lsr #10]
    add r1, r7, #4
    mov r2, #4
    bl record_bytes
    b record_done

record_str:
    mov r1, r10, lsl #20
    mov r1, r1, lsr #20
    bl record_address
    and r9, r10, #0x0000F000
    cmp r9, #0x0000E000
    moveq r9, #0x0000D000
    ldr r0, [sp, r9, lsr #10]
    tst r10, #0x00400000
    moveq r2, #4
    movne r2, #1
    bl record_bytes
    b record_done

record_stm:
    // r6 = registers, r5 = how many
    mov r6, r10, lsl #16
    mov r6, r6, lsr #16
    mov r5, #0
    mov r0, r6
1:
    movs r0, r0, lsr #1
    adc r5, r5, #0
    bne 1b
    // the base before the store: back out the writeback
    tst r10, #0x00200000
    beq 2f
    tst r10, #0x00800000
    subne r8, r8, r5, lsl #2
    addeq r8, r8, r5, lsl #2
2:
    // the lowest address: ia base, ib base + 4, db base - 4n, da base - 4n + 4
    tst r10, #0x00800000
    beq 5f
    tst r10, #0x01000000
    addne r8, r8, #4
    b 6f
5:
    sub r8, r8, r5, lsl #2
    tst r10, #0x01000000
    addeq r8, r8, #4
6:
    mov r4, #0
3:
    cmp r4, #13
    bhs record_done
    tst r6, #1
    beq 4f
    ldr r0, [sp, r4, lsl #2]
    mov r1, r8
    mov r2, #4
    bl record_bytes
    add r8, r8, #4
4:
    mov r6, r6, lsr #1
    add r4, r4, #1
    b 3b

record_done:
    msr cpsr_f, r11
    ldmfd sp!, {r0-r12}
    ldr lr, [sp], #4
    ldr pc, [sp], #4

// From a thumb stub (ShadowInstall.cpp):
//     push {lr} / <the store> / blx shadow_record_thumb / <the instruction after it> / pop {pc}
// lr = the stub + 8, thumb. The store's registers are r0-r7.
.global shadow_record_thumb
.type shadow_record_thumb, %function
shadow_record_thumb:
    stmfd sp!, {r0-r12, lr}
    mrs r11, cpsr
    bic r10, lr, #1
    ldrh r10, [r10, #-6] // the store
    mov r9, r10, lsr #11
    cmp r9, #0x18
    beq thumb_stm
    and r0, r10, #7
    ldr r0, [sp, r0, lsl #2] // the value
    mov r1, r10, lsr #3
    and r1, r1, #7
    ldr r1, [sp, r1, lsl #2] // the base
    mov r2, r10, lsr #6
    and r2, r2, #31
    cmp r9, #0x0C // str: imm5 * 4
    moveq r2, r2, lsl #2
    addeq r1, r1, r2
    moveq r2, #4
    beq 1f
    cmp r9, #0x10 // strh: imm5 * 2
    moveq r2, r2, lsl #1
    addeq r1, r1, r2
    moveq r2, #2
    addne r1, r1, r2 // strb
    movne r2, #1
1:
    bl record_bytes
    b thumb_done
thumb_stm:
    // stmia rb!, {list}: the base went up by 4 a register
    and r6, r10, #0xFF
    mov r5, #0
    mov r0, r6
2:
    movs r0, r0, lsr #1
    adc r5, r5, #0
    bne 2b
    mov r8, r10, lsr #8
    and r8, r8, #7
    ldr r8, [sp, r8, lsl #2]
    sub r8, r8, r5, lsl #2
    mov r4, #0
3:
    tst r6, #1
    beq 4f
    ldr r0, [sp, r4, lsl #2]
    mov r1, r8
    mov r2, #4
    bl record_bytes
    add r8, r8, #4
4:
    mov r6, r6, lsr #1
    add r4, r4, #1
    cmp r4, #8
    blo 3b
thumb_done:
    msr cpsr_f, r11
    ldmfd sp!, {r0-r12, lr}
    bx lr

// From a copy or fill's wrapper (ShadowInstall.cpp):
//     stmfd sp!, {r0-r2, lr} / bl <the routine> / bl shadow_copied_<kind> / .word <the call>
// so [sp] = r0 (the source, or the value), [sp + 4] = r1 (the destination),
// [sp + 8] = r2 (the size), [sp + 12] = where the call goes back to. After a
// call r0-r3, r12 and the flags are the callee's to change.
.global shadow_copied
.type shadow_copied, %function
shadow_copied:
    mov r12, #0
    b copied
.global shadow_filled8
shadow_filled8:
    mov r12, #1
    b copied
.global shadow_filled16
shadow_filled16:
    mov r12, #2
    b copied
.global shadow_filled32
shadow_filled32:
    mov r12, #4
copied:
    stmfd sp!, {r4-r8}
    mov r7, r12
    ldr r4, [sp, #20]
    ldr r5, [sp, #24]
    ldr r6, [sp, #28]
    sub r3, r5, #0x04000000
    cmp r3, #0x2000
    bhs 9f
    cmp r6, #0x400
    movhi r6, #0x400
    cmp r7, #0
    moveq r8, #1
    movne r8, r7
1:
    subs r6, r6, r8
    blt 9f
    cmp r7, #0
    ldreqb r0, [r4], #1
    movne r0, r4
    mov r1, r5
    mov r2, r8
    bl record_bytes
    add r5, r5, r8
    b 1b
9:
    ldmfd sp!, {r4-r8}
    ldmfd sp!, {r0-r2, pc}

// The overlays' sites. Overlays that go in the same memory can't be in at once,
// so the loader gives them the same stub slots and an overlay's stubs are made
// here as it comes in. shadow_groups: per overlay with sites, 8 words
//     id, base, its records, its first slot, the hash of its sites as found,
//     how many arm | thumb << 16, thumb pair | thumb with the one before << 16,
//     its copy calls
// The records are halfwords: a site's offset from the base, in words for the
// arm ones and halfwords for the rest, by type in that order; each takes the
// next slot. What a site holds is read from it once the hash says they all
// still hold what the scan found. The copy calls, whose stubs are made at boot:
//     how many, then per call: the site, the halfwords found, the halfwords to put

// From the overlay hook (ShadowOverlayHook.s), as an overlay has loaded: r0 = its
// table entry.
.global shadow_overlay_loaded
.type shadow_overlay_loaded, %function
shadow_overlay_loaded:
    push {r4, r5, lr}
    ldr r1, [r0] // the overlay's id
    ldr r4, shadow_groups
    ldr r5, shadow_group_count
1:
    subs r5, r5, #1
    blt 9f
    ldr r2, [r4]
    cmp r2, r1
    addne r4, r4, #32
    bne 1b
    mov r0, r4
    bl apply_group
9:
    pop {r4, r5, pc}

// From the exit combo's vblank hook, where no FS_StartOverlay hook went in:
// keeps r4-r11. An overlay goes in once every site reads as found: one half
// read in, or still compressed, is left for a later vblank, and a patched one
// no longer reads as found.
.global shadow_vblank
.type shadow_vblank, %function
shadow_vblank:
    push {r4-r8, lr}
    ldr r4, shadow_groups
    ldr r5, shadow_group_count
1:
    subs r5, r5, #1
    blt 9f
    mov r0, r4
    bl group_hash
    ldr r1, [r4, #16]
    cmp r0, r1
    bne 8f
    ldr r6, [r4, #28]
    ldr r7, [r6], #4
2:
    subs r7, r7, #1
    blt 3f
    ldmia r6!, {r0, r1, r2}
    ldrh r2, [r0]
    ldrh r3, [r0, #2]
    orr r2, r2, r3, lsl #16
    cmp r2, r1
    bne 8f
    b 2b
3:
    mov r0, r4
    bl apply_group
8:
    add r4, r4, #32
    b 1b
9:
    pop {r4-r8, pc}

// r0 = a group -> r0 = the hash of what its sites hold (fnv-1a by words)
group_hash:
    push {r4-r8, lr}
    ldr r4, [r0, #4]
    ldr r5, [r0, #8]
    add r6, r0, #20
    ldr r0, =2166136261
    ldr r8, =16777619
    mov r7, #0
1:
    ldrh r3, [r6], #2
2:
    subs r3, r3, #1
    blt 3f
    ldrh r1, [r5], #2
    cmp r7, #0
    addeq r1, r4, r1, lsl #2
    addne r1, r4, r1, lsl #1
    ldrh r2, [r1]
    ldrh r1, [r1, #2]
    orr r2, r2, r1, lsl #16
    eor r0, r0, r2
    mul r0, r8, r0
    b 2b
3:
    add r7, r7, #1
    cmp r7, #4
    blt 1b
    pop {r4-r8, pc}

// r0 = a group: if its sites hold what was found, each gets its stub and its
// branch; each copy call still as found gets its branch.
apply_group:
    push {r4-r11, lr}
    push {r0}
    mov r11, r0
    bl group_hash
    ldr r1, [r11, #16]
    cmp r0, r1
    bne 6f
    ldr r4, [r11, #4] // the base
    ldr r5, [r11, #8] // the records
    ldr r6, [r11, #12] // the first slot
    add r11, r11, #20 // the counts
    mov r9, #0 // the type
1:
    ldrh r0, [r11], #2
    str r0, [sp, #-4]!
2:
    ldr r0, [sp]
    subs r0, r0, #1
    blt 4f
    str r0, [sp]
    ldrh r7, [r5], #2
    cmp r9, #0
    addeq r7, r4, r7, lsl #2
    addne r7, r4, r7, lsl #1
    ldrh r8, [r7]
    ldrh r0, [r7, #2]
    orr r8, r8, r0, lsl #16
    bl build_stub
    mcr p15, 0, r6, c7, c10, 1 // the slot's line out to memory, for the code fetch
    bl put_site
    add r6, r6, #16
    b 2b
4:
    add sp, sp, #4
    cmp r9, #2
    bgt 6f
    moveq r9, #4
    addlt r9, r9, #1
    b 1b
6:
    ldr r0, [sp]
    ldr r5, [r0, #28]
    ldr r11, [r5], #4
7:
    subs r11, r11, #1
    blt 8f
    ldmia r5!, {r7, r8, r10}
    ldrh r0, [r7]
    ldrh r1, [r7, #2]
    orr r0, r0, r1, lsl #16
    cmp r0, r8
    bne 7b
    bl put_site
    b 7b
8:
    add sp, sp, #4
    mov r0, #0
    mcr p15, 0, r0, c7, c10, 4 // drain the write buffer
    mcr p15, 0, r0, c7, c5, 0 // the instruction cache, as the code may have been there before
    pop {r4-r11, pc}

// r7 = the site, r10 = its halfwords: put, and out to memory
put_site:
    strh r10, [r7]
    mov r0, r10, lsr #16
    strh r0, [r7, #2]
    mcr p15, 0, r7, c7, c10, 1
    add r0, r7, #2
    mcr p15, 0, r0, c7, c10, 1
    bx lr

// r6 = the slot, r7 = the site, r8 = the halfwords found, r9 = the type -> the
// stub in the slot (as ShadowInstall.cpp makes the static ones), r10 = the
// halfwords for the site.
build_stub:
    push {lr}
    cmp r9, #0
    bne 2f
    // arm: the store / str lr, [sp, #-8]! / bl shadow_record / the site + 4
    str r8, [r6]
    ldr r0, =0xE52DE008
    str r0, [r6, #4]
    adrl r1, shadow_record
    add r0, r6, #16
    sub r0, r1, r0
    mov r0, r0, lsl #6
    mov r0, r0, lsr #8
    orr r0, r0, #0xEB000000
    str r0, [r6, #8]
    add r0, r7, #4
    str r0, [r6, #12]
    // the site: b<its condition> to the slot
    add r0, r7, #8
    sub r0, r6, r0
    mov r0, r0, lsl #6
    mov r0, r0, lsr #8
    and r10, r8, #0xF0000000
    orr r10, r10, #0x0A000000
    orr r10, r10, r0
    pop {pc}
2:
    adrl r1, shadow_record_thumb
    cmp r9, #4
    beq 4f
    // push {lr} / the store / blx record / the next (/ blx record) / pop {pc}
    mov r0, r8, lsl #16
    orr r0, r0, #0xB500
    str r0, [r6]
    add r0, r6, #4
    mov r2, #1
    bl thumb_call
    str r0, [r6, #4]
    mov r3, r8, lsr #16
    cmp r9, #1
    bne 3f
    orr r0, r3, #0xBD000000
    str r0, [r6, #8]
    mov r0, #0
    str r0, [r6, #12]
    b 6f
3:
    add r0, r6, #10
    mov r2, #1
    bl thumb_call
    orr r2, r3, r0, lsl #16
    str r2, [r6, #8]
    mov r0, r0, lsr #16
    orr r0, r0, #0xBD000000
    str r0, [r6, #12]
    b 6f
4:
    // push {lr} / the one before / the store / blx record / pop {pc}; the one
    // before a pc-relative ldr: from the stub's last word instead
    mov r0, #0
    str r0, [r6, #12]
    mov r0, r8, lsl #16
    mov r2, r8, lsl #16
    mov r2, r2, lsr #27
    cmp r2, #0x09
    bne 5f
    and r2, r8, #0xFF
    add r12, r7, #4
    bic r12, r12, #3
    ldr r12, [r12, r2, lsl #2]
    str r12, [r6, #12]
    and r0, r8, #0x0700
    orr r0, r0, #0x4800
    orr r0, r0, #2
    mov r0, r0, lsl #16
5:
    orr r0, r0, #0xB500
    str r0, [r6]
    add r0, r6, #6
    mov r2, #1
    bl thumb_call
    mov r3, r8, lsr #16
    orr r2, r3, r0, lsl #16
    str r2, [r6, #4]
    mov r0, r0, lsr #16
    orr r0, r0, #0xBD000000
    str r0, [r6, #8]
6:
    // the site: bl to the slot
    mov r0, r7
    mov r1, r6
    mov r2, #0
    bl thumb_call
    mov r10, r0
    pop {pc}

// r0 = from, r1 = to, r2 = 1 for blx (to arm) -> r0 = the two halfwords
// (first | second << 16). Keeps r1, r3.
thumb_call:
    add r0, r0, #4
    cmp r2, #0
    bicne r0, r0, #3
    sub r0, r1, r0
    mov r12, r0, lsl #9
    mov r12, r12, lsr #21
    orr r12, r12, #0xF000
    mov r0, r0, lsl #20
    mov r0, r0, lsr #21
    cmp r2, #0
    orreq r0, r0, #0xF800
    orrne r0, r0, #0xE800
    orr r0, r12, r0, lsl #16
    bx lr
.ltorg

.global shadow_groups
shadow_groups:
    .word 0
.global shadow_group_count
shadow_group_count:
    .word 0

// r8 = the base after the store, r10 = the store, r1 = its offset -> r7 = r1 = the
// address it wrote. Pre without writeback: base +- off; pre with writeback: the
// base; post: base -+ off.
record_address:
    tst r10, #0x01000000
    beq 1f
    tst r10, #0x00200000
    movne r7, r8
    bne 2f
    tst r10, #0x00800000
    addne r7, r8, r1
    subeq r7, r8, r1
    b 2f
1:
    tst r10, #0x00800000
    subne r7, r8, r1
    addeq r7, r8, r1
2:
    mov r1, r7
    bx lr

// r0 = value, r1 = address, r2 = bytes. Uses r0-r3, r12. A word to a dma
// channel's control that starts it at once also records what the dma writes.
record_bytes:
    cmp r2, #4
    bne record_io
    sub r3, r1, #0x04000000
    subs r3, r3, #0xB8
    cmpne r3, #0x0C
    cmpne r3, #0x18
    cmpne r3, #0x24
    bne record_io
    tst r0, #0x80000000 // enabled
    beq record_io
    tst r0, #0x38000000 // and at once
    bne record_io
    push {r0-r2, lr}
    bl record_dma
    pop {r0-r2, lr}
record_io:
    adrl r12, shadow_data
1:
    sub r3, r1, #0x04000000
    cmp r3, #0x2000
    bhs 5f
    cmp r3, #0x1000
    blo 2f
    sub r3, r3, #0x1000
    sub r3, r3, #0x10
    cmp r3, #0x48
    bhs 5f
    add r3, r3, #0x48
    b 4f
2:
    sub r3, r3, #0x10
    cmp r3, #0x48
    blo 4f
    sub r3, r3, #(0x330 - 0x10)
    cmp r3, #0x90
    bhs 5f
    add r3, r3, #0x90
4:
    strb r0, [r12, r3]
    add r3, r3, #(SHADOW_FLAGS - SHADOW_DATA)
    strb r12, [r12, r3] // the flag: never 0, the shadow is at 0x10 past a 32-byte boundary
5:
    mov r0, r0, ror #8
6:
    add r1, r1, #1
    subs r2, r2, #1
    bne 1b
    bx lr

// r0 = the control word, r1 = its address: the source and destination read
// back on the arm9. Each unit the dma writes into the io pages goes through
// record_io, at most 0x400 of them. Uses r0-r3, r12.
record_dma:
    push {r4-r9, lr}
    ldr r4, [r1, #-8] // source
    ldr r5, [r1, #-4] // destination
    sub r3, r5, #0x04000000
    cmp r3, #0x2000
    bhs 9f
    sub r3, r4, #0x02000000 // a source the game's protection unit lets the cpu read
    cmp r3, #0x06000000
    bhs 9f
    mov r6, r0, lsl #11
    movs r6, r6, lsr #11 // units: 0 is the most
    moveq r6, #0x200000
    cmp r6, #0x400
    movhi r6, #0x400
    tst r0, #0x04000000
    moveq r7, #2
    movne r7, #4
    mov r3, r0, lsr #21 // destination: up, down, fixed, up
    and r3, r3, #3
    cmp r3, #1
    rsbeq r8, r7, #0
    movne r8, r7
    cmp r3, #2
    moveq r8, #0
    mov r3, r0, lsr #23 // source: up, down, fixed
    and r3, r3, #3
    cmp r3, #1
    rsbeq r9, r7, #0
    movne r9, r7
    cmp r3, #2
    moveq r9, #0
1:
    cmp r7, #4
    ldreq r0, [r4]
    ldrhne r0, [r4]
    mov r1, r5
    mov r2, r7
    bl record_io
    add r4, r4, r9
    add r5, r5, r8
    subs r6, r6, #1
    bne 1b
9:
    pop {r4-r9, pc}

.balign 4
.global patch_shadow_end
patch_shadow_end:
.end
