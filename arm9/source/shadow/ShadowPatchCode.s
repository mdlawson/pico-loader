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

// From the overlay hook (ShadowOverlayHook.s), as an overlay has loaded: r0 = its
// table entry. Puts in the overlay's patches the loader made at boot, each a
// halfword: address, then the halfword it should find | the one to put << 16.
.global shadow_overlay_loaded
.type shadow_overlay_loaded, %function
shadow_overlay_loaded:
    push {r4-r6}
    ldr r1, [r0] // the overlay's id
    ldr r2, shadow_groups
    ldr r3, shadow_group_count
1:
    subs r3, r3, #1
    blt 9f
    ldmia r2!, {r4-r6} // id, the patches, how many
    cmp r4, r1
    bne 1b
2:
    subs r6, r6, #1
    blt 3f
    ldmia r5!, {r2, r3} // the address, find | put << 16
    ldrh r12, [r2]
    mov r0, r3, lsl #16
    cmp r12, r0, lsr #16
    moveq r3, r3, lsr #16
    strheq r3, [r2]
    mcreq p15, 0, r2, c7, c10, 1 // the line out to memory, for the code fetch
    b 2b
3:
    // the instruction cache, as the patched code may have been there before
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0
9:
    pop {r4-r6}
    bx lr

// From the exit combo's vblank hook, where no FS_StartOverlay hook went in:
// keeps r4-r11. An overlay's patches go in once all of its patched halfwords
// read as either what the loader found or what it puts there: an overlay half
// read in, or still compressed, is left for a later vblank.
.global shadow_vblank
.type shadow_vblank, %function
shadow_vblank:
    push {r4-r8, lr}
    ldr r4, shadow_groups
    ldr r5, shadow_group_count
1:
    subs r5, r5, #1
    blt 9f
    ldmia r4!, {r0-r2} // id, the patches, how many
    // the first one still as found: the overlay came (back) since
    ldmia r1, {r3, r12}
    ldrh r0, [r3]
    mov r8, r12, lsl #16
    cmp r0, r8, lsr #16
    bne 1b
    mov r6, r1
    mov r7, r2
2:
    ldmia r6!, {r3, r12}
    ldrh r0, [r3]
    mov r8, r12, lsl #16
    cmp r0, r8, lsr #16
    cmpne r0, r12, lsr #16
    bne 1b
    subs r7, r7, #1
    bne 2b
3:
    ldmia r1!, {r3, r12}
    ldrh r0, [r3]
    mov r8, r12, lsl #16
    cmp r0, r8, lsr #16
    moveq r12, r12, lsr #16
    strheq r12, [r3]
    mcreq p15, 0, r3, c7, c10, 1
    subs r2, r2, #1
    bne 3b
    mov r0, #0
    mcr p15, 0, r0, c7, c10, 4 // drain the write buffer
    mcr p15, 0, r0, c7, c5, 0 // and the instruction cache
    b 1b
9:
    pop {r4-r8, lr}
    bx lr

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
