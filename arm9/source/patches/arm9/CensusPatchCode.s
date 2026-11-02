.cpu arm946e-s
.syntax unified
.arm

// Arm9 register-write census: counts the game's writes to the io registers, to
// measure what tracking the write-only ones would cost. The protection unit makes
// the io pages (0x04000000-0x04001FFF) read-only; each store there takes a data
// abort, which the bios hands to the debug vector, and the handler here does the
// store itself, counts it and returns past it. Anything it can't do the same way
// the cpu would turns the census off and runs the instruction again, as the game
// set the protection unit up.
//
// Copied whole into the census space (CENSUS_SPACE_SIZE, from the arm7's main
// memory arena) by the loader: variables, then code, then the arrays at
// CENSUS_ARRAYS. The menu reads the variables and arrays at the offsets below
// (rtsmenu/census.c).
//
// The bios (FFFF0000h vectors, all but irq and swi to one routine): in abort mode
// with irq and fiq masked, it stores the cp15 control, spsr, r12 and lr_abt at
// 027FFD8Ch, 90h, 94h and 98h (sp below the vector, at 027FFD9Ch), clears the
// protection unit enable bit, and calls [027FFD9Ch] with lr = its return. That
// return puts back the control register, spsr and r12 and resumes at the stored
// lr_abt - 4. For a data abort lr_abt is the faulting instruction + 8.

// variables
.equ V_R0,          0x00 // the game's r0-r11 while the handler runs
.equ V_BIOSLR,      0x30
.equ V_OP,          0x34 // opcode of the trapped store
.equ V_PCF,         0x38 // its address, bit 0 set for thumb
.equ V_VC0,         0x3C // VCOUNT as the handler started
.equ V_STATE,       0x40 // 0 off, 1 trapping, 2 no region to use, 3 stopped
.equ V_WANT,        0x44 // the menu's: 1 on, 0 off
.equ V_K,           0x48 // protection region used
.equ V_ORIG_REGION, 0x4C
.equ V_ORIG_DPERM,  0x50
.equ V_ORIG_DCACHE, 0x54
.equ V_ORIG_ICACHE, 0x58
.equ V_ORIG_WBUF,   0x5C
.equ V_NIB_RO,      0x60 // data permission nibble of region k: read-only
.equ V_NIB_RW,      0x64 // and read-write
.equ V_NIB_MASK,    0x68
.equ V_OLDVEC,      0x6C // the debug vector as it was
.equ V_VECADDR,     0x70 // address of the debug vector (loader)
.equ V_BSTACK,      0x74 // address of the bios's words, vector - 0x10 (loader)
.equ V_TRAPS,       0x78
.equ V_TRAPS_LAST,  0x7C // V_TRAPS at the last vblank
.equ V_FRAMES,      0x80
.equ V_MAXF,        0x84 // most traps in one frame
.equ V_HIST,        0x88 // 8 words: frames with 0, 1-15, 16-63, ..., 16384+ traps
.equ V_CAL_TRAP,    0xA8 // lines for CAL_N trapped writes
.equ V_CAL_BASE,    0xAC // lines for CAL_N plain ones
.equ V_CAL_N,       0xB0
.equ V_LINES,       0xB4 // lines crossed inside the handler, summed
.equ V_STOP_REASON, 0xB8 // 1 unsupported store, 2 not an io store, 3 game changed the mpu, 4 cpu mode
.equ V_STOP_PC,     0xBC
.equ V_STOP_OP,     0xC0
.equ V_THUMB,       0xC4 // traps from thumb code
.equ V_STM,         0xC8 // traps from a store multiple
.equ V_SITE_OVF,    0xCC // traps whose site found no slot
.equ V_COUNTING,    0xD0 // 0 while calibrating
.equ V_TICK_LR,     0xD4
.equ V_TICK_SAVE,   0xD8 // r4-r8, 5 words
.equ V_CTRL,        0xEC // cp15 control at install
.equ V_INSTALLS,    0xF0
.equ V_ARRAYS,      0xF4 // address of the arrays (loader)
.equ V_WBREG,       0xF8 // writeback register, -1 for none
.equ V_WBVAL,       0xFC
.equ V_UNITS,       0x100 // words or halfwords the store wrote
.equ V_KIND,        0x104
.equ V_MAGIC,       0x108 // 'CNS1' (loader)
.equ VARS_SIZE,     0x120

// arrays, from V_ARRAYS
.equ A_SITES,       0x000 // 128 x {pc | thumb, count}
.equ A_REGS,        0x400 // 0x1DD words: 0x000-0x6FF, 0x1000-0x106F per word, then the rest
.equ A_MPU,         0xB80 // regions 0-7, dperm, iperm, dcache, icache, wbuf, control at install

.equ CAL_N,         0x400
.equ OUR_REGION,    0x04000019 // base 0x04000000, 8 KB, enabled

.section "patch_census", "ax"
.global patch_census_start
patch_census_start:
census_vars:
    .space V_OLDVEC
census_oldvec:
    .word 0
    .space VARS_SIZE - V_OLDVEC - 4

// ---------------------------------------------------------------- vblank tick
// From the exit combo hook on every vblank: irq mode, irqs masked, the mpu on.
// May use r0-r3, r12 and lr; keeps r4-r11.
.global patch_census_tick_entry
.type patch_census_tick_entry, %function
patch_census_tick_entry:
    adr r12, census_vars
    str lr, [r12, #V_TICK_LR]
    add r3, r12, #V_TICK_SAVE
    stmia r3, {r4-r8}
    mov r8, r12
    // Keep the rts info pointing here (block + 0x164, the vector is at block
    // + 0x71C): the loader's own entry has been seen lost by the time a game runs.
    ldr r0, [r8, #V_VECADDR]
    sub r0, r0, #0x5C0
    str r8, [r0, #8]
    ldr r0, [r8, #V_STATE]
    ldr r1, [r8, #V_WANT]
    cmp r0, #1
    bne tick_notOn
    cmp r1, #0
    beq tick_off
    bl verify
    cmp r0, #0
    beq tick_frame
    // the game set the region or the vector itself: leave it what it made
    mov r0, #0
    str r0, [r8, #V_STOP_PC]
    str r0, [r8, #V_STOP_OP]
    mov r0, #3
    str r0, [r8, #V_STOP_REASON]
    bl uninstall
    mov r0, #3
    str r0, [r8, #V_STATE]
    b tick_done

tick_frame:
    ldr r0, [r8, #V_TRAPS]
    ldr r1, [r8, #V_TRAPS_LAST]
    str r0, [r8, #V_TRAPS_LAST]
    sub r0, r0, r1 // traps this frame
    ldr r1, [r8, #V_FRAMES]
    add r1, r1, #1
    str r1, [r8, #V_FRAMES]
    ldr r1, [r8, #V_MAXF]
    cmp r0, r1
    strhi r0, [r8, #V_MAXF]
    clz r1, r0
    rsbs r1, r1, #32 // significant bits, 0 for none
    mov r2, #0
    beq 1f
    cmp r1, #4
    movls r2, #1
    subhi r2, r1, #1
    movhi r2, r2, lsr #1
    cmp r2, #7
    movhi r2, #7
1:
    add r2, r8, r2, lsl #2
    ldr r1, [r2, #V_HIST]
    add r1, r1, #1
    str r1, [r2, #V_HIST]
    b tick_done

tick_notOn:
    cmp r1, #0
    beq tick_done
    cmp r0, #0
    bleq install
    b tick_done

tick_off:
    bl uninstall
    mov r0, #0
    str r0, [r8, #V_STATE]

tick_done:
    add r3, r8, #V_TICK_SAVE
    ldr lr, [r8, #V_TICK_LR]
    ldmia r3, {r4-r8}
    bx lr

// ---------------------------------------------------------------- region access
// Leaves: r0 = region, r1 = value.
readRegion:
    add pc, pc, r0, lsl #3
    nop
    mrc p15, 0, r1, c6, c0, 0
    bx lr
    mrc p15, 0, r1, c6, c1, 0
    bx lr
    mrc p15, 0, r1, c6, c2, 0
    bx lr
    mrc p15, 0, r1, c6, c3, 0
    bx lr
    mrc p15, 0, r1, c6, c4, 0
    bx lr
    mrc p15, 0, r1, c6, c5, 0
    bx lr
    mrc p15, 0, r1, c6, c6, 0
    bx lr
    mrc p15, 0, r1, c6, c7, 0
    bx lr

writeRegion:
    add pc, pc, r0, lsl #3
    nop
    mcr p15, 0, r1, c6, c0, 0
    bx lr
    mcr p15, 0, r1, c6, c1, 0
    bx lr
    mcr p15, 0, r1, c6, c2, 0
    bx lr
    mcr p15, 0, r1, c6, c3, 0
    bx lr
    mcr p15, 0, r1, c6, c4, 0
    bx lr
    mcr p15, 0, r1, c6, c5, 0
    bx lr
    mcr p15, 0, r1, c6, c6, 0
    bx lr
    mcr p15, 0, r1, c6, c7, 0
    bx lr

// Leaves, r8 = variables; use r0 and r12 only.
permRO:
    mrc p15, 0, r0, c5, c0, 2
    ldr r12, [r8, #V_NIB_MASK]
    bic r0, r0, r12
    ldr r12, [r8, #V_NIB_RO]
    orr r0, r0, r12
    mcr p15, 0, r0, c5, c0, 2
    bx lr

permRW:
    mrc p15, 0, r0, c5, c0, 2
    ldr r12, [r8, #V_NIB_MASK]
    bic r0, r0, r12
    ldr r12, [r8, #V_NIB_RW]
    orr r0, r0, r12
    mcr p15, 0, r0, c5, c0, 2
    bx lr

// ---------------------------------------------------------------- install
// r8 = variables. Uses r0-r7 and r12, returns through r7.
install:
    mov r7, lr
    // the setup as the game made it, for the report
    ldr r5, [r8, #V_ARRAYS]
    add r5, r5, #A_MPU
    mov r6, #0
1:
    mov r0, r6
    bl readRegion
    str r1, [r5, r6, lsl #2]
    add r6, r6, #1
    cmp r6, #8
    blo 1b
    mrc p15, 0, r0, c5, c0, 2
    str r0, [r5, #32]
    mrc p15, 0, r0, c5, c0, 3
    str r0, [r5, #36]
    mrc p15, 0, r0, c2, c0, 0
    str r0, [r5, #40]
    mrc p15, 0, r0, c2, c0, 1
    str r0, [r5, #44]
    mrc p15, 0, r0, c3, c0, 0
    str r0, [r5, #48]
    mrc p15, 0, r0, c1, c0, 0
    str r0, [r5, #52]
    str r0, [r8, #V_CTRL]

    // the gba slot's region: nothing should need slot 2 while it is ours
    mov r4, #0
2:
    ldr r1, [r5, r4, lsl #2]
    mov r2, r1, lsr #12
    cmp r2, #0x8000
    bne 3f
    tst r1, #1
    bne 4f
3:
    add r4, r4, #1
    cmp r4, #8
    blo 2b
    mov r0, #2
    str r0, [r8, #V_STATE]
    bx r7
4:
    str r4, [r8, #V_K]
    str r1, [r8, #V_ORIG_REGION]
    ldr r0, [r5, #32]
    str r0, [r8, #V_ORIG_DPERM]
    ldr r0, [r5, #40]
    str r0, [r8, #V_ORIG_DCACHE]
    ldr r0, [r5, #44]
    str r0, [r8, #V_ORIG_ICACHE]
    ldr r0, [r5, #48]
    str r0, [r8, #V_ORIG_WBUF]
    mov r0, r4, lsl #2
    mov r1, #0xF
    mov r1, r1, lsl r0
    str r1, [r8, #V_NIB_MASK]
    mov r1, #6 // privileged and user read-only
    mov r1, r1, lsl r0
    str r1, [r8, #V_NIB_RO]
    mov r1, #3 // full access
    mov r1, r1, lsl r0
    str r1, [r8, #V_NIB_RW]

    // The vector first, so the first trap finds the handler. The bios reads it
    // with the mpu off, past the data cache, and so does the handler's chain.
    ldr r0, [r8, #V_VECADDR]
    ldr r1, [r0]
    adr r2, patch_census_abort_entry
    cmp r1, r2
    strne r1, [r8, #V_OLDVEC]
    str r2, [r0]
    mcr p15, 0, r0, c7, c10, 1 // clean the vector's line
    add r0, r8, #V_OLDVEC
    mcr p15, 0, r0, c7, c10, 1
    mov r0, #0
    mcr p15, 0, r0, c7, c10, 4 // drain write buffer

    // region k: the io pages, not cached or buffered, data read-only
    mov r1, #1
    mov r1, r1, lsl r4
    mrc p15, 0, r0, c2, c0, 0
    bic r0, r0, r1
    mcr p15, 0, r0, c2, c0, 0
    mrc p15, 0, r0, c2, c0, 1
    bic r0, r0, r1
    mcr p15, 0, r0, c2, c0, 1
    mrc p15, 0, r0, c3, c0, 0
    bic r0, r0, r1
    mcr p15, 0, r0, c3, c0, 0
    mov r0, r4
    mov r1, #0x04000000
    orr r1, r1, #0x19
    bl writeRegion
    bl permRO

    mov r0, #1
    str r0, [r8, #V_STATE]
    ldr r0, [r8, #V_INSTALLS]
    add r0, r0, #1
    str r0, [r8, #V_INSTALLS]
    ldr r0, [r8, #V_TRAPS]
    str r0, [r8, #V_TRAPS_LAST]
    ldr r0, [r8, #V_CAL_N]
    cmp r0, #0
    bxne r7
    // fall through: calibrate once

// CAL_N writes of DISPCNT's own value, trapped and then not, timed with VCOUNT.
calibrate:
    mov r0, #0
    str r0, [r8, #V_COUNTING]
    mov r3, #0x04000000
    ldr r2, [r3] // DISPCNT
    ldrh r4, [r3, #6]
    mov r1, #CAL_N
1:
    str r2, [r3]
    subs r1, r1, #1
    bne 1b
    ldrh r5, [r3, #6]
    bl permRW
    ldrh r6, [r3, #6]
    mov r1, #CAL_N
1:
    str r2, [r3]
    subs r1, r1, #1
    bne 1b
    ldrh r1, [r3, #6]
    bl permRO
    subs r5, r5, r4
    addmi r5, r5, #256
    addmi r5, r5, #7
    subs r1, r1, r6
    addmi r1, r1, #256
    addmi r1, r1, #7
    str r5, [r8, #V_CAL_TRAP]
    str r1, [r8, #V_CAL_BASE]
    mov r0, #CAL_N
    str r0, [r8, #V_CAL_N]
    mov r0, #1
    str r0, [r8, #V_COUNTING]
    bx r7

// ---------------------------------------------------------------- verify
// r0 = 0 while region k, its permission and the vector are still ours.
verify:
    mov r7, lr
    ldr r0, [r8, #V_K]
    bl readRegion
    mov r2, #0x04000000
    orr r2, r2, #0x19
    cmp r1, r2
    bne 9f
    mrc p15, 0, r1, c5, c0, 2
    ldr r2, [r8, #V_NIB_MASK]
    and r1, r1, r2
    ldr r2, [r8, #V_NIB_RO]
    cmp r1, r2
    bne 9f
    ldr r0, [r8, #V_VECADDR]
    ldr r1, [r0]
    adr r2, patch_census_abort_entry
    cmp r1, r2
    bne 9f
    mov r0, #0
    bx r7
9:
    mov r0, #1
    bx r7

// ---------------------------------------------------------------- uninstall
// Puts back each part only while it is still the census's own. Uses r0-r7 and
// r12, returns through r7.
uninstall:
    mov r7, lr
    ldr r4, [r8, #V_K]
    mov r0, r4
    bl readRegion
    mov r2, #0x04000000
    orr r2, r2, #0x19
    cmp r1, r2
    bne 2f
    mov r0, r4
    ldr r1, [r8, #V_ORIG_REGION]
    bl writeRegion
    mov r1, #1
    mov r1, r1, lsl r4
    ldr r2, [r8, #V_ORIG_DCACHE]
    mrc p15, 0, r0, c2, c0, 0
    bic r0, r0, r1
    and r3, r2, r1
    orr r0, r0, r3
    mcr p15, 0, r0, c2, c0, 0
    ldr r2, [r8, #V_ORIG_ICACHE]
    mrc p15, 0, r0, c2, c0, 1
    bic r0, r0, r1
    and r3, r2, r1
    orr r0, r0, r3
    mcr p15, 0, r0, c2, c0, 1
    ldr r2, [r8, #V_ORIG_WBUF]
    mrc p15, 0, r0, c3, c0, 0
    bic r0, r0, r1
    and r3, r2, r1
    orr r0, r0, r3
    mcr p15, 0, r0, c3, c0, 0
    // the permission nibble, if it is one of ours
    mrc p15, 0, r0, c5, c0, 2
    ldr r1, [r8, #V_NIB_MASK]
    and r2, r0, r1
    ldr r3, [r8, #V_NIB_RO]
    cmp r2, r3
    ldrne r3, [r8, #V_NIB_RW]
    cmpne r2, r3
    bne 2f
    bic r0, r0, r1
    ldr r2, [r8, #V_ORIG_DPERM]
    and r2, r2, r1
    orr r0, r0, r2
    mcr p15, 0, r0, c5, c0, 2
2:
    ldr r0, [r8, #V_VECADDR]
    ldr r1, [r0]
    adr r2, patch_census_abort_entry
    cmp r1, r2
    ldreq r1, [r8, #V_OLDVEC]
    streq r1, [r0]
    mcr p15, 0, r0, c7, c10, 1
    mov r0, #0
    mcr p15, 0, r0, c7, c10, 4
    bx r7

// ---------------------------------------------------------------- abort
// From the bios: abort mode (undefined mode for an undefined instruction), irq
// and fiq masked, the mpu off, lr = into the bios, r12 free (the bios keeps the
// game's). Nothing but cp15 and registers until the mpu is back on: with it off
// a load would miss what the data cache holds.
.global patch_census_abort_entry
.type patch_census_abort_entry, %function
patch_census_abort_entry:
    mrs r12, cpsr
    and r12, r12, #0x1F
    cmp r12, #0x17
    bne chainOff
    mrc p15, 0, r12, c1, c0, 0
    orr r12, r12, #1
    mcr p15, 0, r12, c1, c0, 0
    adrl r12, census_vars
    stmia r12, {r0-r11}
    mov r8, r12
    str lr, [r8, #V_BIOSLR]
    mov r0, #0x04000000
    ldrh r1, [r0, #6]
    str r1, [r8, #V_VC0]
    ldr r0, [r8, #V_STATE]
    cmp r0, #1
    bne chainOn

    ldr r10, [r8, #V_BSTACK] // +0 control, +4 spsr, +8 r12, +12 lr_abt
    ldr r0, [r10, #12]
    sub r9, r0, #8 // the faulting instruction
    ldr r1, [r10, #4]
    and r2, r1, #0x20
    orr r0, r9, r2, lsr #5
    str r0, [r8, #V_PCF]
    // the banked registers of the game's mode are reached by switching to it
    and r1, r1, #0x1F
    cmp r1, #0x11 // fiq: r8-r12 banked too
    cmpne r1, #0x17
    cmpne r1, #0x1B
    moveq r0, #4
    beq stopRetry
    mov r0, #-1
    str r0, [r8, #V_WBREG]
    tst r2, r2
    bne thumbDecode

    ldr r7, [r9]
    str r7, [r8, #V_OP]
    and r0, r7, #0x0E000000
    cmp r0, #0x08000000
    beq armStm
    and r0, r7, #0x0C000000
    cmp r0, #0x04000000
    beq armStr
    ands r0, r7, #0x0E000000
    bne unsupported
    and r0, r7, #0x90
    cmp r0, #0x90
    bne unsupported
    tst r7, #0x60
    beq unsupported // swp, multiplies
    tst r7, #0x00100000
    bne foreign // a load
    and r0, r7, #0x60
    cmp r0, #0x20
    moveq r6, #1 // strh
    beq 1f
    cmp r0, #0x60
    bne foreign // ldrd
    mov r6, #4 // strd
    and r0, r7, #0xF000
    tst r0, #0x1000
    bne unsupported // odd first register
    cmp r0, #0xE000
    beq unsupported
1:
    tst r7, #0x00400000
    andne r5, r7, #0xF
    andne r0, r7, #0xF00
    orrne r5, r5, r0, lsr #4
    bne armAddress
    and r0, r7, #0xF
    cmp r0, #15
    beq unsupported
    bl getReg
    mov r5, r0
    b armAddress

armStr:
    tst r7, #0x00100000
    bne foreign
    tst r7, #0x00400000
    movne r6, #2 // strb
    moveq r6, #0 // str
    tst r7, #0x02000000
    moveq r0, r7, lsl #20
    moveq r5, r0, lsr #20
    beq armAddress
    tst r7, #0x10
    bne unsupported
    and r0, r7, #0xF
    cmp r0, #15
    beq unsupported
    bl getReg // r0 = Rm
    mov r1, r7, lsr #7
    and r1, r1, #0x1F // amount
    mov r2, r7, lsr #5
    and r2, r2, #3 // type
    cmp r2, #0
    moveq r5, r0, lsl r1
    beq armAddress
    cmp r2, #1
    bne 1f
    cmp r1, #0
    moveq r5, #0
    movne r5, r0, lsr r1
    b armAddress
1:
    cmp r2, #2
    bne 2f
    cmp r1, #0
    moveq r5, r0, asr #31
    movne r5, r0, asr r1
    b armAddress
2:
    cmp r1, #0
    movne r5, r0, ror r1
    bne armAddress
    ldr r2, [r10, #4] // rrx with the game's carry
    and r2, r2, #0x20000000
    mov r5, r0, lsr #1
    orr r5, r5, r2, lsl #2

// r5 = offset, r6 = kind (0 word, 1 half, 2 byte, 4 double)
armAddress:
    mov r0, r7, lsr #16
    and r0, r0, #0xF
    cmp r0, #15
    beq unsupported
    mov r11, r0
    bl getReg
    mov r3, r0 // base
    tst r7, #0x00800000
    addne r2, r3, r5
    subeq r2, r3, r5
    tst r7, #0x01000000
    moveq r4, r3 // post-indexed: the base, then always written back
    movne r4, r2
    beq 1f
    tst r7, #0x00200000
    beq 2f
1:
    str r11, [r8, #V_WBREG]
    str r2, [r8, #V_WBVAL]
2:
    mov r0, r7, lsr #12
    and r5, r0, #0xF
    cmp r5, #15
    beq unsupported
    b checkRange

armStm:
    tst r7, #0x00100000
    bne foreign
    tst r7, #0x00400000
    bne unsupported // user bank
    mov r0, r7, lsl #16
    movs r5, r0, lsr #16
    beq unsupported
    tst r5, #0x8000
    bne unsupported
    mov r0, r7, lsr #16
    and r0, r0, #0xF
    cmp r0, #15
    beq unsupported
    mov r11, r0
    bl getReg
    mov r3, r0 // base
    bl popcount // r1 = registers in r5
    mov r6, #3
    str r1, [r8, #V_UNITS]
    tst r7, #0x00800000
    addne r2, r3, r1, lsl #2 // the base after
    subeq r2, r3, r1, lsl #2
    // first address: ia base, ib base + 4, da base - 4n + 4, db base - 4n
    tst r7, #0x00800000
    beq 1f
    mov r4, r3
    tst r7, #0x01000000
    addne r4, r4, #4
    b 2f
1:
    mov r4, r2
    tst r7, #0x01000000
    addeq r4, r4, #4
2:
    tst r7, #0x00200000
    strne r11, [r8, #V_WBREG]
    strne r2, [r8, #V_WBVAL]
    b checkRange

thumbDecode:
    ldrh r7, [r9]
    str r7, [r8, #V_OP]
    mov r0, r7, lsr #11
    cmp r0, #0x0C // str rd, [rn, #imm5 * 4]
    moveq r6, #0
    moveq r5, #2
    beq thumbImm
    cmp r0, #0x0E // strb
    moveq r6, #2
    moveq r5, #0
    beq thumbImm
    cmp r0, #0x10 // strh
    moveq r6, #1
    moveq r5, #1
    beq thumbImm
    cmp r0, #0x12 // str rd, [sp, #imm8 * 4]
    beq thumbSp
    cmp r0, #0x18 // stmia rn!, {list}
    beq thumbStm
    mov r0, r7, lsr #9
    cmp r0, #0x28
    moveq r6, #0
    beq thumbReg
    cmp r0, #0x29
    moveq r6, #1
    beq thumbReg
    cmp r0, #0x2A
    moveq r6, #2
    beq thumbReg
    b unsupported

thumbImm: // r5 = scale
    mov r0, r7, lsr #6
    and r0, r0, #0x1F
    mov r4, r0, lsl r5
    mov r0, r7, lsr #3
    and r0, r0, #7
    bl getReg
    add r4, r4, r0
    and r5, r7, #7
    b checkRange

thumbSp:
    mov r0, #13
    bl getReg
    and r4, r7, #0xFF
    add r4, r0, r4, lsl #2
    mov r5, r7, lsr #8
    and r5, r5, #7
    mov r6, #0
    b checkRange

thumbReg:
    mov r0, r7, lsr #6
    and r0, r0, #7
    bl getReg
    mov r4, r0
    mov r0, r7, lsr #3
    and r0, r0, #7
    bl getReg
    add r4, r4, r0
    and r5, r7, #7
    b checkRange

thumbStm:
    ands r5, r7, #0xFF
    beq unsupported
    mov r0, r7, lsr #8
    and r0, r0, #7
    mov r11, r0
    bl getReg
    mov r4, r0
    bl popcount
    str r1, [r8, #V_UNITS]
    add r2, r4, r1, lsl #2
    str r11, [r8, #V_WBREG]
    str r2, [r8, #V_WBVAL]
    mov r6, #3

// r4 = first address, r5 = source register or list, r6 = kind
checkRange:
    str r6, [r8, #V_KIND]
    cmp r6, #3
    ldreq r1, [r8, #V_UNITS]
    moveq r1, r1, lsl #2
    beq 1f
    cmp r6, #4
    moveq r1, #2
    movne r1, #1
    str r1, [r8, #V_UNITS]
    cmp r6, #4
    moveq r1, #8
    beq 1f
    cmp r6, #0
    moveq r1, #4
    beq 1f
    cmp r6, #1
    moveq r1, #2
    movne r1, #1
1:
    sub r0, r4, #0x04000000
    cmp r0, #0x2000
    bhs foreign
    add r0, r0, r1
    cmp r0, #0x2000
    bhi foreign

    bl permRW
    cmp r6, #3
    beq storeMultiple
    mov r0, r5
    bl getReg
    cmp r6, #0
    biceq r1, r4, #3
    streq r0, [r1]
    cmp r6, #1
    biceq r1, r4, #1
    strheq r0, [r1]
    cmp r6, #2
    strbeq r0, [r4]
    cmp r6, #4
    bne stored
    bic r1, r4, #3
    str r0, [r1]
    add r0, r5, #1
    bl getReg
    bic r1, r4, #3
    str r0, [r1, #4]
    b stored

storeMultiple:
    bic r1, r4, #3
    mov r2, r5
    mov r3, #0
1:
    movs r2, r2, lsr #1
    bcc 2f
    mov r0, r3
    bl getReg
    str r0, [r1], #4
2:
    add r3, r3, #1
    cmp r2, #0
    bne 1b

stored:
    bl permRO
    ldr r0, [r8, #V_WBREG]
    cmn r0, #1
    ldrne r1, [r8, #V_WBVAL]
    blne setReg

    ldr r0, [r8, #V_COUNTING]
    cmp r0, #0
    beq handlerReturn
    ldr r0, [r8, #V_TRAPS]
    add r0, r0, #1
    str r0, [r8, #V_TRAPS]
    ldr r1, [r8, #V_PCF]
    tst r1, #1
    ldrne r0, [r8, #V_THUMB]
    addne r0, r0, #1
    strne r0, [r8, #V_THUMB]
    ldr r0, [r8, #V_KIND]
    cmp r0, #3
    ldreq r0, [r8, #V_STM]
    addeq r0, r0, #1
    streq r0, [r8, #V_STM]

    // per register word
    ldr r5, [r8, #V_ARRAYS]
    add r6, r5, #A_REGS
    ldr r3, [r8, #V_UNITS]
    ldr r0, [r8, #V_KIND]
    cmp r0, #4
    moveq r3, #2
    bic r2, r4, #3
1:
    sub r1, r2, #0x04000000
    cmp r1, #0x700
    movlo r1, r1, lsr #2
    blo 2f
    sub r1, r1, #0x1000
    cmp r1, #0x70
    movlo r1, r1, lsr #2
    addlo r1, r1, #0x1C0
    movhs r1, #0x1DC
2:
    ldr r0, [r6, r1, lsl #2]
    add r0, r0, #1
    str r0, [r6, r1, lsl #2]
    add r2, r2, #4
    subs r3, r3, #1
    bhi 1b

    // per site: open addressing over 128 slots, 16 tries
    ldr r1, [r8, #V_PCF]
    mov r2, r1, lsr #1
    and r2, r2, #127
    mov r3, #16
1:
    add r12, r5, r2, lsl #3
    ldr r0, [r12]
    cmp r0, r1
    beq 3f
    cmp r0, #0
    streq r1, [r12]
    beq 3f
    add r2, r2, #1
    and r2, r2, #127
    subs r3, r3, #1
    bne 1b
    ldr r0, [r8, #V_SITE_OVF]
    add r0, r0, #1
    str r0, [r8, #V_SITE_OVF]
    b 4f
3:
    ldr r0, [r12, #4]
    add r0, r0, #1
    str r0, [r12, #4]
4:
    // lines crossed while in here: summed, an estimate of the time taken
    mov r0, #0x04000000
    ldrh r0, [r0, #6]
    ldr r1, [r8, #V_VC0]
    subs r0, r0, r1
    addmi r0, r0, #256
    addmi r0, r0, #7
    ldr r1, [r8, #V_LINES]
    add r1, r1, r0
    str r1, [r8, #V_LINES]

handlerReturn:
    // the bios resumes at the lr it kept - 4: past the store for arm, which
    // that already is, and for thumb 2 bytes past it
    ldr r0, [r8, #V_PCF]
    tst r0, #1
    addne r0, r9, #6
    strne r0, [r10, #12]
    ldr lr, [r8, #V_BIOSLR]
    ldmia r8, {r0-r11}
    bx lr

unsupported:
    mov r0, #1
    b stopRetry
foreign:
    mov r0, #2
// The census ends here: put the game's setup back and run the instruction again.
stopRetry:
    str r0, [r8, #V_STOP_REASON]
    ldr r1, [r8, #V_PCF]
    str r1, [r8, #V_STOP_PC]
    ldr r1, [r8, #V_OP]
    str r1, [r8, #V_STOP_OP]
    bl uninstall
    mov r0, #3
    str r0, [r8, #V_STATE]
    add r0, r9, #4
    str r0, [r10, #12]
    ldr lr, [r8, #V_BIOSLR]
    ldmia r8, {r0-r11}
    bx lr

// not trapping: on to the vector the game had, as the bios left things
chainOn:
    ldmia r8, {r0-r11}
    mrc p15, 0, r12, c1, c0, 0
    bic r12, r12, #1
    mcr p15, 0, r12, c1, c0, 0
chainOff:
    ldr r12, census_oldvec // cleaned to memory at install, read past the cache
    cmp r12, #0
    bxeq lr
    bx r12

// ---------------------------------------------------------------- registers
// Leaves, r8 = variables, r10 = the bios's words. getReg: r0 = index -> value;
// setReg: r0 = index, r1 = value. Use r12. Never 15.
getReg:
    cmp r0, #12
    ldrlo r0, [r8, r0, lsl #2]
    bxlo lr
    ldreq r0, [r10, #8]
    bxeq lr
    ldr r12, [r10, #4]
    and r12, r12, #0x1F
    cmp r12, #0x10
    moveq r12, #0x1F // user: through system mode
    orr r12, r12, #0xC0
    msr cpsr_c, r12
    cmp r0, #13
    moveq r0, sp
    movne r0, lr
    msr cpsr_c, #0xD7
    bx lr

setReg:
    cmp r0, #12
    strlo r1, [r8, r0, lsl #2]
    bxlo lr
    streq r1, [r10, #8]
    bxeq lr
    ldr r12, [r10, #4]
    and r12, r12, #0x1F
    cmp r12, #0x10
    moveq r12, #0x1F
    orr r12, r12, #0xC0
    msr cpsr_c, r12
    cmp r0, #13
    moveq sp, r1
    movne lr, r1
    msr cpsr_c, #0xD7
    bx lr

// r1 = set bits in r5. Leaf, uses r0.
popcount:
    mov r0, r5
    mov r1, #0
1:
    movs r0, r0, lsr #1
    adc r1, r1, #0
    bne 1b
    bx lr

.global patch_census_end
patch_census_end:

.end
