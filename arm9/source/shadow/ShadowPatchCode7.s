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
//     +0x130 a bit per shadow byte (bit n & 7 of byte n >> 3), set once the
//            game wrote it
//     +0x170 b shadow7_freeze, b shadow7_resume, b shadow7_restart, b
//            shadow7_vblank (the exit combo's, version 3 on)
//     +0x180 the frozen timers, +0x184 their counts, +0x194 the line they
//            froze on, +0x198 frames, +0x19C the freeze's time, +0x1A0 the
//            channels the restart holds, +0x1A4 those the vblank starts,
//            +0x1A8 each channel's start time (16 words)
//     then this code, then the stubs
//
// Time is in scanlines: the vblanks the exit combo counted * 263 + the lines
// since the last. It stops while the menu runs. A channel's start is stamped
// as the game writes its control's top byte (its setup, the start follows at
// once); the menu works
// out from it where a loaded state's pcm channels were, seeks them there or
// has them start later (from the top of their sample, a loop or more before
// they get back to where they were), and this side starts them on time.
//
// Arm stubs only, as on the arm9: <the store> / str lr, [sp, #-8]! /
// bl shadow7_record / .word the site + 4.

.equ SHADOW_DATA,   0x10
.equ SHADOW_FLAGS,  0x130
.equ FLAGS_SIZE,    0x40

.section "patch_shadow7", "ax"
.global patch_shadow7_start
patch_shadow7_start:
    .word 0x37444853 // 'SHD7'
    .word 3
    .word 0
    .word 0
shadow7_data:
    .space 0x120
shadow7_flags:
    .space FLAGS_SIZE

    b shadow7_freeze
    b shadow7_resume
    b shadow7_restart
    b shadow7_vblank
shadow7_frozen:
    .word 0
shadow7_counts: // count | control << 16
    .space 16
shadow7_line: // VCOUNT as they froze
    .word 0
shadow7_frames:
    .word 0
shadow7_frozeAt:
    .word 0
shadow7_hold: // channels the restart leaves off (the menu's)
    .word 0
shadow7_delayed: // channels the vblank starts at their time
    .word 0
shadow7_started: // each channel's start time
    .space 64

// From the exit combo's vblank irq, IF acknowledged: counts the frame, and
// starts the delayed channels due before the next one, each on its line.
shadow7_vblank:
    adr r0, shadow7_frames
    ldr r1, [r0]
    add r1, r1, #1
    str r1, [r0], #(shadow7_delayed - shadow7_frames)
    ldr r2, [r0]
    cmp r2, #0
    bxeq lr
    push {r4-r6, lr}
1:
    mov r1, #0
    bl s7_time
    ldr r4, [r0, #(shadow7_delayed - shadow7_frames)]!
    mov r5, #0 // one due later this frame
    mov r6, #0
2:
    mov r1, #1
    tst r4, r1, lsl r6
    beq 3f
    add r2, r0, #(shadow7_started - shadow7_delayed)
    ldr r2, [r2, r6, lsl #2]
    subs r2, r2, r3
    bgt 4f
    bic r4, r4, r1, lsl r6
    mov r2, #0x04000000
    add r2, r2, #0x400
    add r2, r2, r6, lsl #4
    ldr r1, [r2]
    orr r1, r1, #0x80000000
    str r1, [r2] // on
    b 3f
4:
    cmp r2, #250
    movlt r5, #1
3:
    add r6, r6, #1
    cmp r6, #16
    blo 2b
    str r4, [r0]
    cmp r5, #0
    bne 1b
    pop {r4-r6, pc}

// r3 = now, plus r1 (263 when a vblank is pending and not counted yet); r0 =
// shadow7_frames. Uses r0-r3.
s7_now:
    mov r3, #0x04000000
    ldr r1, [r3, #0x214] // IF
    ands r1, r1, #1
    movne r1, #256
    addne r1, r1, #7
s7_time:
    mov r2, #0x04000000
    ldrh r3, [r2, #6] // VCOUNT
    subs r3, r3, #192
    addlo r3, r3, #256 // lines since the vblank began
    addlo r3, r3, #7
    add r3, r3, r1
    adr r0, shadow7_frames
    ldr r1, [r0]
    add r3, r3, r1, lsl #8
    add r3, r3, r1, lsl #3
    sub r3, r3, r1 // + frames * 263
    bx lr

// From the exit combo as it holds the game: the running timers whose reload
// the shadow knows stop, so the game's tick and alarms go on from where they
// were, not from what passed in the menu. Keeps r4-r11.
shadow7_freeze:
    push {r4-r6, lr}
    adr r2, shadow7_flags
    ldrh r2, [r2, #0x20] // the timers': bit 4 * n their reload
    adr r4, shadow7_counts
    mov r3, #0x04000000
    add r3, r3, #0x100
    mov r0, #0
    mov r6, #0
1:
    tst r2, #1
    mov r2, r2, lsr #4
    beq 2f
    ldr r1, [r3, r6, lsl #2] // count | control << 16
    tst r1, #0x00800000 // running
    beq 2f
    str r1, [r4, r6, lsl #2]
    mov r1, r1, lsr #16
    bic r1, r1, #0x80
    add r5, r3, r6, lsl #2
    strh r1, [r5, #2] // stopped
    mov r1, #1
    orr r0, r0, r1, lsl r6
2:
    add r6, r6, #1
    cmp r6, #4
    blo 1b
    adr r1, shadow7_frozen
    str r0, [r1]
    ldrh r0, [r3, #-0xFA] // VCOUNT, 0x04000006
    str r0, [r1, #(shadow7_line - shadow7_frozen)]
    mov r1, #0 // in the vblank irq, its frame counted (its IF may still show)
    bl s7_time
    str r3, [r0, #(shadow7_frozeAt - shadow7_frames)]
    pop {r4-r6, pc}

// As the game goes on (after a load, this is the state's): each frozen timer
// starts from its count, then gets its reload back for the next overflow. An
// overflow irq that wasn't pending as it froze (r1 = IF then) goes.
shadow7_resume:
    push {r4-r8, lr}
    adr r4, shadow7_frozen
    ldr r0, [r4]
    mov r5, #0
    str r5, [r4], #4 // r4: the counts
    adrl r2, shadow7_data
    add r2, r2, #0x100 // the reloads
    mov r3, #0x04000000
    add r3, r3, #0x100
    // on the line they froze on, so they keep their phase to the display
    cmp r0, #0
    beq 9f
    ldr r5, [r4, #(shadow7_line - shadow7_counts)]
3:
    ldrh r6, [r3, #-0xFA] // VCOUNT: out of the line first, then into it at its start
    cmp r6, r5
    beq 3b
3:
    ldrh r6, [r3, #-0xFA]
    cmp r6, r5
    bne 3b
    mov r6, #0
1:
    tst r0, #1
    beq 2f
    ldr r5, [r4, r6, lsl #2]
    add r7, r3, r6, lsl #2
    strh r5, [r7] // the count, as the reload
    mov r5, r5, lsr #16
    bic r8, r5, #0x80
    strh r8, [r7, #2] // stopped
    strh r5, [r7, #2] // started: it counts on from there
    add r8, r2, r6, lsl #2
    ldrh r8, [r8]
    strh r8, [r7] // the reload, from the next overflow
    mov r8, #8
    mov r8, r8, lsl r6 // its IF bit
    tst r1, r8
    bne 2f
    add r7, r3, #0x114 // 0x04000214
    str r8, [r7]
2:
    mov r0, r0, lsr #1
    add r6, r6, #1
    cmp r6, #4
    blo 1b
9:
    pop {r4-r8, pc}

// After a load (this is the state's), r1 = restartMask: bits 0-15 the channels
// the state had playing. The timer reloads and the capture registers go back,
// then each of those channels gets its sample address, timer, loop start and
// length and starts again, from the top of its sample: where it was isn't kept
// anywhere. Keeps r4-r11.
shadow7_restart:
    push {r4-r10, lr}
    adr r2, patch_shadow7_start
    ldr r0, [r2, #(shadow7_hold - patch_shadow7_start)]
    bic r0, r1, r0 // not the held channels: the menu's, or the vblank's later
    mov r1, #0
    str r1, [r2, #(shadow7_hold - patch_shadow7_start)]
    add r4, r2, #0x110 // the timers' data
    ldr r5, [r2, #(SHADOW_FLAGS + 0x20)] // and flags: timer n bit 4 * n, capture bit 16
    mov r6, #0x04000000
    add r6, r6, #0x100
    mov r7, #4
1:
    tst r5, #1
    mov r5, r5, lsr #4
    ldrhne r8, [r4]
    strhne r8, [r6] // TMxCNT_L: the reload, the count goes on
    add r4, r4, #4
    add r6, r6, #4
    subs r7, r7, #1
    bne 1b
    // capture: r4 = data + 0x110, r5 bit 0 its flag
    mov r6, #0x04000000
    add r6, r6, #0x500
    tst r5, #1
    beq 2f
    ldr r8, [r4]
    str r8, [r6, #0x10] // SNDCAP0DAD
    ldrh r8, [r4, #4]
    strh r8, [r6, #0x14] // SNDCAP0LEN
    ldr r8, [r4, #8]
    str r8, [r6, #0x18] // SNDCAP1DAD
    ldrh r8, [r4, #12]
    strh r8, [r6, #0x1C] // SNDCAP1LEN
    ldrh r8, [r6, #8] // SNDCAP0CNT/1CNT, as the load set them: start them over
    bic r9, r8, #0x8000
    bic r9, r9, #0x0080
    cmp r9, r8
    strhne r9, [r6, #8]
    strhne r8, [r6, #8]
2:
    add r4, r2, #0x10 // channel 0's data
    add r5, r2, #0x130 // and flags, a halfword each
    sub r6, r6, #0x100 // SOUND0CNT
    ldr r7, [r2, #(shadow7_delayed - patch_shadow7_start)]
3:
    ldrh r9, [r5]
    tst r0, #1
    tsteq r7, #1
    beq 4f
    tst r9, #0x10
    ldrne r8, [r4, #4]
    strne r8, [r6, #4] // SAD
    tst r9, #0x100
    ldrhne r8, [r4, #8]
    strhne r8, [r6, #8] // TMR
    tst r9, #0x400
    ldrhne r8, [r4, #10]
    strhne r8, [r6, #10] // PNT
    tst r9, #0x1000
    ldrne r8, [r4, #12]
    strne r8, [r6, #12] // LEN
    tst r0, #1
    ldrne r8, [r6]
    orrne r8, r8, #0x80000000
    strne r8, [r6] // and on
4:
    add r4, r4, #16
    add r5, r5, #2
    add r6, r6, #16
    mov r7, r7, lsr #1
    mov r0, r0, lsr #1
    add r1, r2, #(SHADOW_FLAGS + 0x20)
    cmp r5, r1 // past channel 15's flags
    bne 3b
    pop {r4-r10, pc}

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

// r0 = value, r1 = address, r2 = bytes. Uses r0-r3, r7, r9, r10, r12.
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
    add r3, r3, #0x410 // o - 0x100
    cmp r3, #0x10
    bhs 5f
    add r3, r3, #0x100 // the timers: 0x100-0x10F
4:
    strb r0, [r12, r3]
    // the flag; r7, r9 and r10 are free in shadow7_record by now
    add r7, r12, #(SHADOW_FLAGS - SHADOW_DATA)
    and r10, r3, #7
    mov r9, #1
    mov r10, r9, lsl r10
    ldrb r9, [r7, r3, lsr #3]
    orr r9, r9, r10
    strb r9, [r7, r3, lsr #3]
    cmp r3, #0x100
    bhs 5f
    and r9, r3, #0xF
    cmp r9, #3 // a channel's control's top byte: the game's now, not a delayed start
    bne 5f
    push {r0-r3, lr}
    mov r9, r3, lsr #4
    ldr r10, [r12, #(shadow7_delayed - shadow7_data)]
    mov r1, #1
    bic r10, r10, r1, lsl r9
    str r10, [r12, #(shadow7_delayed - shadow7_data)]
    // stamped with or without the start bit: the sdk sets that bit through a
    // store no stub sees, in the same tick as this, its setup (volume and pan
    // have stores of their own)
    bl s7_now
    add r0, r0, #(shadow7_started - shadow7_frames)
    str r3, [r0, r9, lsl #2]
    pop {r0-r3, lr}
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
