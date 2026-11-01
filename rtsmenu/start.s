.cpu arm946e-s
.syntax unified
.arm

// Entry and exit of the real-time save menu, run by the arm9 at RTS_BASE.
//
// The arm7 has stashed RTS_STASH_SIZE bytes of main memory from here to the sd
// card, loaded this file in their place and answered RUN. The arm9 exit combo
// wait piece jumped here from the game's vblank irq, irqs off, with:
//   r0 = the wait piece entry, to return to
//   r1 = exit_request_block_t
//   r2 = 0, r3 = 0x04000000, lr = back into the hook
// The game's r4-r11 and irq stack pointer have to be kept. Everything up to
// RTS_BASE + RTS_STASH_SIZE is ours; the layout is in rtsmenu.ld.
//
// rtsMain (menu.c) runs the menu and returns the answer, RTS_MENU_STATE_RESUME
// or _EXIT; this file then does the handshake with the arm7 and goes back.

.equ MENU_STATE,        0x128   // exit_request_block_t::rts.menuState
.equ COMBO_MAGIC,       0x0C    // exit_request_block_t::comboMagic
.equ RUN_MAGIC,         0x4B524152
.equ STATE_EXIT,        0x54495845
.equ STATE_LEFT,        0x5446454C

.section .text.start, "ax"
.global _start
_start:
    adr r12, gSavedRegs
    stmia r12, {r0, r1, r4-r11, lr}
    str sp, [r12, #44]
    ldr sp, =__stack_top

    // zero .bss
    ldr r2, =__bss_start
    ldr r3, =__bss_end
    mov r0, #0
1:
    cmp r2, r3
    strlo r0, [r2], #4
    blo 1b

    // r0 = the block
    ldr r0, [r12, #4]
    bl rtsMain
    mov r5, r0

    // Answer the arm7, then wait for it to take the answer.
    adr r12, gSavedRegs
    ldr r1, [r12, #4] // block
    mov r3, #0x04000000
    str r5, [r1, #MENU_STATE]
    add r6, r1, #MENU_STATE
    mov r2, #0
    mcr p15, 0, r6, c7, c14, 1 // clean and invalidate the line
    mcr p15, 0, r2, c7, c10, 4 // drain write buffer
    ldr r7, =RUN_MAGIC
2:
    mcr p15, 0, r1, c7, c6, 1 // invalidate the line, the arm7 has no cache
    ldr r2, [r1, #COMBO_MAGIC]
    cmp r2, r7
    beq 2b

    // An exit goes through the reboot code, which hands over to the arm7 through
    // the ipc sync: start it from 0, as the check piece does for an exit request.
    ldr r2, =STATE_EXIT
    cmp r5, r2
    moveq r2, #0
    strbeq r2, [r3, #0x181] // REG_IPC_SYNC data out

    // The way back, held in registers: from the moment the arm7 sees LEFT it puts
    // the game's memory back over this, so nothing here may be touched after the
    // cache flush below, not even read. The game's r4-r11 come back as well: its
    // irq handler runs on after the wait piece.
    ldr r2, [r12, #44]
    mov sp, r2 // the game's irq stack
    ldmia r12, {r0, r1, r4-r11, lr}
    add r3, r1, #MENU_STATE
    ldr r12, =STATE_LEFT

    // Write back and drop the whole data cache, 4 ways of 32 lines of 32 bytes:
    // what this menu left in it must not land on the game's memory later.
    mov r2, #0
3:
    mcr p15, 0, r2, c7, c14, 2
    add r2, r2, #0x20
    tst r2, #0x400
    beq 3b
    bic r2, r2, #0x400
    adds r2, r2, #0x40000000
    bcc 3b
    b leave

.pool

// One instruction cache line, fetched as a whole before LEFT is stored.
.balign 32
leave:
    str r12, [r1, #MENU_STATE]
    mcr p15, 0, r3, c7, c10, 1 // clean the line
    mcr p15, 0, r2, c7, c10, 4 // drain write buffer (r2 = 0)
    mov r3, #0x04000000
    bx r0 // the wait piece, with r1 = block, r2 = 0, r3 = io, lr = hook
    nop
    nop
    nop

.balign 4
.global gSavedRegs
gSavedRegs:
    .space 12 * 4 // r0, r1, r4-r11, lr, sp

// void readBankedRegs(u32* out): spsr_irq, then sp, lr, spsr of svc, abt and
// und, sp and lr of sys, r8-r12, sp, lr and spsr of fiq. Called in irq mode
// with irqs off; every mode switch keeps them off.
.global readBankedRegs
.type readBankedRegs, %function
readBankedRegs:
    mrs r2, cpsr
    mrs r1, spsr
    str r1, [r0], #4
    msr cpsr_c, #0xD3 // svc
    str sp, [r0], #4
    str lr, [r0], #4
    mrs r1, spsr
    str r1, [r0], #4
    msr cpsr_c, #0xD7 // abt
    str sp, [r0], #4
    str lr, [r0], #4
    mrs r1, spsr
    str r1, [r0], #4
    msr cpsr_c, #0xDB // und
    str sp, [r0], #4
    str lr, [r0], #4
    mrs r1, spsr
    str r1, [r0], #4
    msr cpsr_c, #0xDF // sys
    str sp, [r0], #4
    str lr, [r0], #4
    msr cpsr_c, #0xD1 // fiq
    stmia r0!, {r8-r12, sp, lr}
    mrs r1, spsr
    str r1, [r0], #4
    msr cpsr_c, r2
    bx lr

// void writeBankedRegs(const u32* in): readBankedRegs' layout, back into the
// registers. Called in irq mode with irqs off.
.global writeBankedRegs
.type writeBankedRegs, %function
writeBankedRegs:
    mrs r2, cpsr
    ldr r1, [r0], #4
    msr spsr_cxsf, r1
    msr cpsr_c, #0xD3 // svc
    ldr sp, [r0], #4
    ldr lr, [r0], #4
    ldr r1, [r0], #4
    msr spsr_cxsf, r1
    msr cpsr_c, #0xD7 // abt
    ldr sp, [r0], #4
    ldr lr, [r0], #4
    ldr r1, [r0], #4
    msr spsr_cxsf, r1
    msr cpsr_c, #0xDB // und
    ldr sp, [r0], #4
    ldr lr, [r0], #4
    ldr r1, [r0], #4
    msr spsr_cxsf, r1
    msr cpsr_c, #0xDF // sys
    ldr sp, [r0], #4
    ldr lr, [r0], #4
    msr cpsr_c, #0xD1 // fiq
    ldmia r0!, {r8-r12, sp, lr}
    ldr r1, [r0], #4
    msr spsr_cxsf, r1
    msr cpsr_c, r2
    bx lr

.section .rodata.font, "a"
.balign 4
.global gFont
gFont:
    .incbin "font.bin"
