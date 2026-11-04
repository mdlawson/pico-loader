.cpu arm7tdmi
.syntax unified
.arm

// The arm7's half of the trap-free register shadow (ShadowInstall7.cpp): the
// sound channels' sample address, timer, loop start and length, the timer
// reloads and the capture registers are write only, so a load can't put back
// what a state's sound was playing. Each store the loader found in the arm7
// gets a stub that also keeps the value here, and the exit combo's arm7 half
// starts the state's channels again from it when the menu leaves.
//
// The space (after the arm9's shadow, in main memory a load restores):
//     +0x000 'SHD7', +0x004 version, +0x008 stubs, +0x00C offset of the stubs
//     +0x010 the shadow: 0x120 bytes, 0x04000400-4FF at 0, 0x04000100-10F at
//            0x100, 0x04000510-51F at 0x110
//     +0x130 a byte per shadow byte, not 0 once the game wrote it
//     +0x250 this code, then the stubs
//
// Arm stubs only, as on the arm9: <the store> / str lr, [sp, #-8]! /
// bl shadow7_record / .word the site + 4.

.equ SHADOW_DATA,   0x10
.equ SHADOW_FLAGS,  0x130

.section "patch_shadow7", "ax"
.global patch_shadow7_start
patch_shadow7_start:
    .word 0x37444853 // 'SHD7'
    .word 1
    .word 0
    .word 0
shadow7_data:
    .space 0x120
shadow7_flags:
    .space 0x120

// From a stub: lr = the stub + 12, [sp] = the caller's lr, [sp + 4] free.
.global shadow7_record
.type shadow7_record, %function
shadow7_record:
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
    beq 7f
    tst r10, #0x0C000000
    bne 6f
    // strh: imm8 split in two nibbles
    and r1, r10, #0xF
    and r0, r10, #0xF00
    orr r1, r1, r0, lsr #4
    bl 1f
    and r9, r10, #0x0000F000
    cmp r9, #0x0000E000
    moveq r9, #0x0000D000
    ldr r0, [sp, r9, lsr #10]
    mov r2, #2
    bl 2f
    b 9f
6:  // str / strb
    mov r1, r10, lsl #20
    mov r1, r1, lsr #20
    bl 1f
    and r9, r10, #0x0000F000
    cmp r9, #0x0000E000
    moveq r9, #0x0000D000
    ldr r0, [sp, r9, lsr #10]
    tst r10, #0x00400000
    moveq r2, #4
    movne r2, #1
    bl 2f
    b 9f
7:  // stm: r6 = registers, r5 = how many
    mov r6, r10, lsl #16
    mov r6, r6, lsr #16
    mov r5, #0
    mov r0, r6
3:
    movs r0, r0, lsr #1
    adc r5, r5, #0
    bne 3b
    tst r10, #0x00200000 // the base before the store: back out the writeback
    beq 3f
    tst r10, #0x00800000
    subne r8, r8, r5, lsl #2
    addeq r8, r8, r5, lsl #2
3:
    tst r10, #0x00800000 // the lowest address
    beq 4f
    tst r10, #0x01000000
    addne r8, r8, #4
    b 5f
4:
    sub r8, r8, r5, lsl #2
    tst r10, #0x01000000
    addeq r8, r8, #4
5:
    mov r4, #0
3:
    cmp r4, #13
    bhs 9f
    tst r6, #1
    beq 4f
    ldr r0, [sp, r4, lsl #2]
    mov r1, r8
    mov r2, #4
    bl 2f
    add r8, r8, #4
4:
    mov r6, r6, lsr #1
    add r4, r4, #1
    b 3b
9:
    msr cpsr_f, r11
    ldmfd sp!, {r0-r12}
    ldr lr, [sp], #4
    ldr pc, [sp], #4

// r1 = offset, r8 = base after the store, r10 = the store: r1 = r7 = address
1:
    tst r10, #0x01000000
    beq 3f
    tst r10, #0x00200000
    movne r7, r8
    bne 4f
    tst r10, #0x00800000
    addne r7, r8, r1
    subeq r7, r8, r1
    b 4f
3:
    tst r10, #0x00800000
    subne r7, r8, r1
    addeq r7, r8, r1
4:
    mov r1, r7
    bx lr

// r0 = value, r1 = address, r2 = bytes. Uses r0-r3, r12.
2:
    adrl r12, shadow7_data
3:
    sub r3, r1, #0x04000000
    sub r3, r3, #0x400 // o - 0x400
    cmp r3, #0x100
    blo 4f // a channel: 0-0xFF
    sub r3, r3, #0x110 // o - 0x510
    cmp r3, #0x10
    addlo r3, r3, #0x110
    blo 4f // capture: 0x110-0x11F
    add r3, r3, #0x410
    sub r3, r3, #0x100 // o - 0x100
    cmp r3, #0x10
    bhs 5f
    add r3, r3, #0x100 // the timers: 0x100-0x10F
4:
    strb r0, [r12, r3]
    add r3, r3, #(SHADOW_FLAGS - SHADOW_DATA)
    strb r12, [r12, r3] // the flag: never 0, the shadow is 0x10 past a 32-byte boundary
5:
    mov r0, r0, ror #8
    add r1, r1, #1
    subs r2, r2, #1
    bne 3b
    bx lr

.balign 4
.global patch_shadow7_end
patch_shadow7_end:
.end
