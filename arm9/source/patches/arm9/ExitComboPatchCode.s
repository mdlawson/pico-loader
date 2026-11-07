.cpu arm946e-s
.syntax unified
.arm

// Arm9 half of the in-game exit to the launcher, in four pieces so each one fits
// a gap the arm9 patch heap has left after every mandatory patch. A piece never
// reaches another one pc-relative: every jump between pieces goes through a
// literal the loader fills, as each piece lands at its own address.
//
// Registers: the bios saved r0-r3, r12 and lr before entering the dispatcher, and
// the dispatcher tail the hook displaces loads r1 and lr itself, so r0-r3, r12 and
// lr are free; r4-r11 must be preserved.
//
// Fixed distances in the shared page, the same in both mirrors (static_asserts in
// ExitComboPatch.cpp): resetParam is block + 0x5A0, the sdk card lock word is
// block + 0x960 (resetParam + 0x3C0).

// ---------------------------------------------------------------- hook
// Reached through ldr pc from the first instruction of the SDK irq dispatcher
// tail, with r0 = irq index. Runs on every irq, so it stays short: only a vblank
// with the arm7's combo magic set goes on to the check piece, which returns here
// with r0 = 0 whenever the game goes on. With the vcount piece in (below), the
// loader makes the first instruction tst r0, #0x1D, so index 2 comes in as well,
// and the check literal leads to that piece, which gives the index back.
.section "patch_exitcombo_hook", "ax"
.global patch_exitcombo_hook_entry
.type patch_exitcombo_hook_entry, %function
patch_exitcombo_hook_entry:
.global patch_exitcombo_hook_first
patch_exitcombo_hook_first: // the loader sets this instruction, see above
    cmp r0, #0 // irq index 0 = vblank (tst r0, #0x1D with the vcount piece: 0 or 2)
    bne 1f
    // the vblank call (the shadow's, or the register-write census through the
    // census gate below), when the loader put one in: keeps r4-r11, gives back r0 = 0.
    // Not on a vcount (r0 = 2): r12 = 0.
    subs r12, r0, #2
    ldrne r12, patch_exitcombo_hook_census_address
    cmp r12, #0
    blxne r12
    ldr r1, patch_exitcombo_hook_block_address
    mcr p15, 0, r1, c7, c6, 1 // invalidate the line, the arm7 has no cache
    ldr r2, [r1, #0x0C] // exit_request_block_t::comboMagic
    ldr r3, hookComboMagic
    cmp r2, r3
    ldrne r3, hookFreezeMagic
    cmpne r2, r3
    ldreq r12, patch_exitcombo_hook_check_address
    blxeq r12
1:
    // the dispatcher tail displaced by the hook
    ldr r1, patch_exitcombo_hook_table_address
    ldr r0, [r1, r0, lsl #2]
    ldr lr, patch_exitcombo_hook_return_address
    bx r0

hookComboMagic:
    .word 0x424D4F43 // EXIT_REQUEST_COMBO_MAGIC
hookFreezeMagic:
    .word 0x435A5246 // EXIT_REQUEST_FREEZE_COMBO_MAGIC
.global patch_exitcombo_hook_block_address
patch_exitcombo_hook_block_address:
    .word 0
.global patch_exitcombo_hook_check_address
patch_exitcombo_hook_check_address:
    .word 0
.global patch_exitcombo_hook_table_address
patch_exitcombo_hook_table_address:
    .word 0
.global patch_exitcombo_hook_return_address
patch_exitcombo_hook_return_address:
    .word 0
.global patch_exitcombo_hook_census_address
patch_exitcombo_hook_census_address:
    .word 0

// ---------------------------------------------------------------- check
// r1 = block, r2 = the published combo magic, r0 = 0, lr = back into the hook.
// Only between card operations: the sdk holds its card lock word for every rom
// read, and the DSpico stops for good when one is cut short, so this waits for a
// vblank where the word is free.
.section "patch_exitcombo_check", "ax"
.global patch_exitcombo_check_entry
.type patch_exitcombo_check_entry, %function
patch_exitcombo_check_entry:
    // EXIT for the exit combo, FREZ for the freeze combo
    ldr r12, checkRequestKey
    eor r12, r12, r2
    add r3, r1, #0x960 // sdk card lock word
    mcr p15, 0, r3, c7, c14, 1 // clean and invalidate the line, should a game cache it
    ldr r2, [r3] // lockFlag, nonzero while held
    cmp r2, #0
    bxne lr // try again on the next vblank

    mov r3, #0x04000000
    str r2, [r3, #0x208] // IME = 0. Per cpu, so the arm7 keeps taking its vblank.
    // The reboot code hands over to the arm7 through this sync, so clear what
    // the game left in it before the arm7 can see the request. Only for an
    // exit: a freeze gives the game back its sync. Bit 0 of the request magic
    // is set in EXIT and clear in FREZ.
    tst r12, #1
    strbne r2, [r3, #0x181] // REG_IPC_SYNC data out

    // Write the whole data cache back, so the arm7 sees the game's main memory
    // as it is: an rts stashes part of it. Clean and invalidate by set/way,
    // 4 ways of 32 lines of 32 bytes; r0 ends at 0 again.
    mov r0, #0
3:
    mcr p15, 0, r0, c7, c14, 2
    add r0, r0, #0x20
    tst r0, #0x400
    beq 3b
    bic r0, r0, #0x400
    adds r0, r0, #0x40000000
    bcc 3b

    str r12, [r1] // exit_request_block_t::requestMagic
    mcr p15, 0, r1, c7, c10, 1 // clean dcache entry by MVA
    mcr p15, 0, r2, c7, c10, 4 // drain write buffer
    ldr pc, patch_exitcombo_check_wait_address // r1 = block, r2 = 0, r3 = io

checkRequestKey:
    .word 0x16041706 // EXIT_REQUEST_REQUEST_KEY: combo magic ^ key = request magic
.global patch_exitcombo_check_wait_address
patch_exitcombo_check_wait_address:
    .word 0

// ---------------------------------------------------------------- wait
// r1 = block, r2 = 0, r3 = 0x04000000, lr = back into the hook.
// Waits for the arm7 to park, or to cancel when it cannot stop safely. The pause
// between reads keeps this off the bus while the arm7 finishes.
.section "patch_exitcombo_wait", "ax"
.global patch_exitcombo_wait_entry
.type patch_exitcombo_wait_entry, %function
patch_exitcombo_wait_entry:
    ldr r0, waitParkMagic
1:
    mov r12, #0x400
2:
    subs r12, r12, #1
    bne 2b
    // Drop what the instruction cache holds before acting on the answer: on RUN
    // the rts menu is where game code may have been cached, and after one it is
    // the other way round.
    mcr p15, 0, r2, c7, c5, 0 // invalidate the whole icache
    mcr p15, 0, r1, c7, c6, 1 // invalidate the line
    ldr r12, [r1, #0x0C] // exit_request_block_t::comboMagic
    subs r12, r12, r0 // 0 = parked, 1 = cancelled (also the end of a freeze), 2 = run
    ldreq pc, patch_exitcombo_wait_teardown_address
    // RUN: the arm7 has loaded the rts menu at rts_info_t::base. It comes back
    // here (r0) with r1-r3 and lr as they are now, once the arm7 took its answer.
    cmp r12, #2
    adreq r0, patch_exitcombo_wait_entry
    ldreq pc, [r1, #0x114] // exit_request_block_t::rts.base
    cmp r12, #1
    bne 1b

    // Cancelled: take the request back and let the game go on, starting with
    // the vblank it is in. Irqs that came meanwhile are still pending.
    str r2, [r1] // exit_request_block_t::requestMagic = 0
    mcr p15, 0, r1, c7, c10, 1 // clean dcache entry by MVA
    mcr p15, 0, r2, c7, c10, 4 // drain write buffer
    mov r0, #1
    str r0, [r3, #0x208] // IME = 1
    mov r0, #0 // irq index 0 = vblank, for the hook's dispatch
    bx lr

waitParkMagic:
    .word 0x4B524150 // EXIT_REQUEST_PARK_MAGIC, EXIT_REQUEST_CANCEL_MAGIC is one more
.global patch_exitcombo_wait_teardown_address
patch_exitcombo_wait_teardown_address:
    .word 0

// ---------------------------------------------------------------- teardown
// r1 = block, r2 = 0, r3 = 0x04000000. Never returns.
.section "patch_exitcombo_teardown", "ax"
.global patch_exitcombo_teardown_entry
.type patch_exitcombo_teardown_entry, %function
patch_exitcombo_teardown_entry:
    // Take the card back: the loader hands slot 1 to the arm7 before booting a
    // game and only the arm9 can undo that. Nothing is in flight on it: the
    // lock word was free and the arm7 parked between its own card accesses.
    add r0, r3, #0x204 // REG_EXMEMCNT, halfword offsets are only 8 bit
    ldrh r12, [r0]
    bic r12, r12, #0x880
    strh r12, [r0]

    // the game side of OS_ResetSystem would have quiesced its own dma
    str r2, [r3, #0xB8] // DMA0CNT
    str r2, [r3, #0xC4] // DMA1CNT
    str r2, [r3, #0xD0] // DMA2CNT
    str r2, [r3, #0xDC] // DMA3CNT

    str r2, [r1, #0x5A0] // resetParam

    // Into the reboot code past its reset of the game's arm7, which ours already
    // parked, with the registers its entry would have loaded from its pool.
    ldr r0, patch_exitcombo_teardown_resetpool_address
    ldmia r0, {r0, r1, r3, r4, r5, r6, r7}
    mov r9, r4 // readSdSectors
    mov r10, r7 // bootPicoLoader
    ldr r12, patch_exitcombo_teardown_bootloaderbinaries_address
    bx r12 // thumb

.global patch_exitcombo_teardown_resetpool_address
patch_exitcombo_teardown_resetpool_address:
    .word 0
.global patch_exitcombo_teardown_bootloaderbinaries_address
patch_exitcombo_teardown_bootloaderbinaries_address:
    .word 0


// ---------------------------------------------------------------- census gate
// Optional, its own piece: the hook calls it with lr = back into the hook, r2 and
// r3 free. Every trap the census takes goes through the bios's exception handler,
// which pushes onto its own stack with the protection unit still on. The ds bios
// keeps that stack at 027FFD9Ch, inside the sdk's shared page region; the dsi bios,
// which a DSi keeps for the exit combo, at 02FFFD9Ch, which no region of a ds game
// covers: the first trap would abort again on that push, for good. So region 2
// becomes that page, read-write and not cached (the same memory as 023FF000h in
// 4 MB mode). Region 2 is the sdk's no-access guard over the arm7's corner of main
// memory; without it that corner is plain main memory (region 1), which also lets
// the census run there when the arm7's arena put it there. Then on into the
// census, which returns to the hook.
.section "patch_census_gate", "ax"
.global patch_census_gate_entry
.type patch_census_gate_entry, %function
patch_census_gate_entry:
    ldr r3, gateStackRegion
    mcr p15, 0, r3, c6, c2, 0
    mrc p15, 0, r2, c5, c0, 2 // data: full access
    bic r2, r2, #0xF00
    orr r2, r2, #0x300
    mcr p15, 0, r2, c5, c0, 2
    mrc p15, 0, r2, c2, c0, 0 // not cached
    bic r2, r2, #4
    mcr p15, 0, r2, c2, c0, 0
    mrc p15, 0, r2, c3, c0, 0 // not buffered
    bic r2, r2, #4
    mcr p15, 0, r2, c3, c0, 0
    push {lr}
    ldr r12, patch_census_gate_target
    blx r12
    mov r0, #0 // the vblank's index, for the hook
    pop {pc}
gateStackRegion:
    .word 0x02FFF017 // base 02FFF000h, 4 KB, enabled
.global patch_census_gate_target
patch_census_gate_target:
    .word 0

// ---------------------------------------------------------------- vcount
// Optional, when the patch heap has room for it after the rest: a game may turn the
// vblank irq off and keep its frames with a vcount one (Mario & Luigi: Bowser's
// Inside Story, once in play), and then never gets a vblank for the hook. With this
// piece in, the hook comes here instead of the check, for a vblank or a vcount
// (r0 = 0 or 2; r1 = block, r2 = the combo magic, lr = back into the hook). A vcount
// goes on only while the vblank irq is off, so other games are as they were. The
// check and wait give back r0 = 0: this gives the hook the index it came with.
.section "patch_exitcombo_vcount", "ax"
.global patch_exitcombo_vcount_entry
.type patch_exitcombo_vcount_entry, %function
patch_exitcombo_vcount_entry:
    cmp r0, #0
    beq 1f
    mov r3, #0x04000000
    ldr r3, [r3, #0x210] // IE
    tst r3, #1
    bxne lr // vblank's on: it takes the combo
1:
    push {r0, lr}
    ldr r12, patch_exitcombo_vcount_check_address
    blx r12
    pop {r0, pc}

.global patch_exitcombo_vcount_check_address
patch_exitcombo_vcount_check_address:
    .word 0

.end
