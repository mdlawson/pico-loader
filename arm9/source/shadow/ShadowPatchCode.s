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

// r0 = value, r1 = address, r2 = bytes. Uses r0-r3, r12.
record_bytes:
    adr r12, shadow_data
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

.balign 4
.global patch_shadow_end
patch_shadow_end:
.end
