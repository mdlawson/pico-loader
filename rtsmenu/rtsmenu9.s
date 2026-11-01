.cpu arm946e-s
.syntax unified
.arm

// Minimal real-time save menu (RTS step 2), run by the arm9 at RTS_BASE.
//
// The arm7 has stashed RTS_STASH_SIZE bytes of main memory from here, loaded this
// file in their place and answered RUN. The arm9 exit combo wait piece jumped
// here from the game's vblank irq, irqs off, with:
//   r0 = the wait piece entry, to return to
//   r1 = exit_request_block_t
//   r2 = 0, r3 = 0x04000000, lr = back into the hook (keep)
// Anything up to RTS_BASE + RTS_STASH_SIZE is ours, including the stack.
//
// For now it only shows plain colours on both screens: A or B resumes the game,
// Select exits to the launcher.

.equ RTS_STASH_SIZE,    0x10000
.equ MENU_STATE,        0x128   // exit_request_block_t::rts.menuState
.equ COMBO_MAGIC,       0x0C    // exit_request_block_t::comboMagic
.equ RUN_MAGIC,         0x4B524152
.equ STATE_RESUME,      0x4D534552
.equ STATE_EXIT,        0x54495845
.equ STATE_LEFT,        0x5446454C

.section .text
.global _start
_start:
    adr r12, savedRegs
    stmia r12, {r0, r1, r4-r11, lr}
    str sp, [r12, #44]
    ldr sp, =_start + RTS_STASH_SIZE

    // the game's screen state
    mov r3, #0x04000000
    add r4, r3, #0x1000 // engine B
    adr r12, savedScreen
    ldr r5, [r3]         // DISPCNT A
    ldr r6, [r4]         // DISPCNT B
    ldr r7, [r3, #0x6C]  // MASTER_BRIGHT A
    ldr r8, [r4, #0x6C]  // MASTER_BRIGHT B
    ldr r9, [r3, #0x50]  // BLDCNT/BLDALPHA A
    ldr r10, [r4, #0x50] // BLDCNT/BLDALPHA B
    add r0, r3, #0x300
    ldrh r11, [r0, #4]   // POWCNT1
    stmia r12!, {r5-r11}
    mov r0, #0x05000000
    ldrh r5, [r0]        // backdrop A
    add r0, r0, #0x400
    ldrh r6, [r0]        // backdrop B
    stmia r12!, {r5, r6}

    // both screens on, every layer off: only the backdrops show
    add r0, r3, #0x300
    orr r11, r11, #0x0003 // lcd, 2d engine A
    orr r11, r11, #0x0200 // 2d engine B
    strh r11, [r0, #4]
    mov r2, #0x10000 // display mode 1, mode 0, no layers
    str r2, [r3]
    str r2, [r4]
    mov r2, #0
    str r2, [r3, #0x6C]
    str r2, [r4, #0x6C]
    str r2, [r3, #0x50]
    str r2, [r4, #0x50]
    mov r0, #0x05000000
    ldr r2, =0x5C63 // a muted blue, top or bottom depending on the game
    strh r2, [r0]
    ldr r2, =0x2D6B // grey
    add r0, r0, #0x400
    strh r2, [r0]
    add r10, r3, #0x130 // KEYINPUT, active low

    // the combo is still held: wait until every key is up
    bl waitRelease
1:
    ldrh r2, [r10]
    and r5, r2, #0x03
    cmp r5, #0x03 // A or B down
    bne 2f
    tst r2, #0x04 // Select
    bne 1b
    ldr r5, =STATE_EXIT
    b 3f
2:
    ldr r5, =STATE_RESUME
3:
    bl waitRelease // the game should not see the key

    // put the screens back
    adr r12, savedScreen
    ldmia r12!, {r6-r11, lr}
    str r6, [r3]
    str r7, [r4]
    str r8, [r3, #0x6C]
    str r9, [r4, #0x6C]
    str r10, [r3, #0x50]
    str r11, [r4, #0x50]
    add r0, r3, #0x300
    strh lr, [r0, #4]
    ldmia r12!, {r6, r7}
    mov r0, #0x05000000
    strh r6, [r0]
    add r0, r0, #0x400
    strh r7, [r0]

    // Answer the arm7, then wait for it to take the answer.
    adr r12, savedRegs
    ldr r1, [r12, #4] // block
    str r5, [r1, #MENU_STATE]
    add r6, r1, #MENU_STATE
    mov r2, #0
    mcr p15, 0, r6, c7, c14, 1 // clean and invalidate the line
    mcr p15, 0, r2, c7, c10, 4 // drain write buffer
    ldr r7, =RUN_MAGIC
4:
    mcr p15, 0, r1, c7, c6, 1 // invalidate the line, the arm7 has no cache
    ldr r2, [r1, #COMBO_MAGIC]
    cmp r2, r7
    beq 4b

    // An exit goes through the reboot code, which hands over to the arm7 through
    // the ipc sync: start it from 0, as the check piece does for an exit request.
    ldr r2, =STATE_EXIT
    cmp r5, r2
    moveq r2, #0
    strbeq r2, [r3, #0x181] // REG_IPC_SYNC data out

    // The way back, held in registers: from the moment the arm7 sees LEFT it puts
    // the game's memory back over this, so nothing here may be touched after the
    // cache flush below, not even read.
    // The game's r4-r11 back as well: its irq handler runs on after the wait piece.
    ldr r2, [r12, #44]
    mov sp, r2 // the game's irq stack
    ldmia r12, {r0, r1, r4-r11, lr}
    add r3, r1, #MENU_STATE
    ldr r12, =STATE_LEFT

    // Write back and drop the whole data cache, 4 ways of 32 lines of 32 bytes:
    // what this menu left in it must not land on the game's memory later.
    mov r2, #0
5:
    mcr p15, 0, r2, c7, c14, 2
    add r2, r2, #0x20
    tst r2, #0x400
    beq 5b
    bic r2, r2, #0x400
    adds r2, r2, #0x40000000
    bcc 5b
    b leave

// r10 = KEYINPUT
waitRelease:
    ldrh r2, [r10]
    mvn r2, r2
    lsls r2, r2, #22 // the ten keys in bits 22-31
    bne waitRelease
    // a little longer, past the bounce
    mov r2, #0x40000
1:
    subs r2, r2, #1
    bne 1b
    bx lr

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
savedRegs:
    .space 12 * 4 // r0, r1, r4-r11, lr, sp
savedScreen:
    .space 9 * 4
