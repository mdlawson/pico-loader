.cpu arm7tdmi
.section "patch_exitcombo_arm7", "ax"
.syntax unified
.arm

// Resident arm7 half of the in-game exit to the launcher.
//
// Reached through ldr pc from the first instruction of the SDK irq dispatcher
// tail, so r0 holds the irq index and the original r1/lr loads did not run yet.
// The bios saved r0-r3, r12 and lr before entering the dispatcher, so those are
// free to use; r4-r11 must be preserved.
//
// A game that samples the microphone runs on the sdk's nested irq handler
// instead, which reaches the handlers straight from the irq table without
// passing the dispatcher tail. Its dispatch point is hooked as well, with the
// handler about to be called already in r0. Both ways in share the detection
// below, and lr tells dispatch which displaced instructions to make up for.
//
// This half owns the whole combo detection, because the arm7 patch heap has
// hundreds of bytes to spare while the arm9 one barely has room for the reboot
// path. It also means a missing arm9 patch cannot hang anything: without it
// nobody ever answers the request and the combo simply does nothing.
.global patch_exitcombo_arm7_prioentry
.type patch_exitcombo_arm7_prioentry, %function
patch_exitcombo_arm7_prioentry:
    ldr r1, patch_exitcombo_arm7_table_address
    ldr r1, [r1] // entry 0 of the irq table, the vblank handler
    cmp r0, r1
    bne prioDispatch
    stmfd sp!, {r0}
    mov r0, #0 // the vblank index, as the dispatcher tail would have it
    mov lr, #1
    b common

.global patch_exitcombo_arm7_entry
.type patch_exitcombo_arm7_entry, %function
patch_exitcombo_arm7_entry:
    mov lr, #0
common:
    cmp r0, #0 // irq index 0 = vblank
    bne dispatch
    push {lr}
    mov r0, #12
    bl shadow7Call // the shadow's clock
    pop {lr}
    mov r0, #0

    // the state words below: hold counter, request age and the save state the
    // wrappers at the end keep; the save cooldown loses a frame every vblank
    adrl r12, holdCounter
    ldr r2, [r12, #16] // saveCooldown
    subs r2, r2, #1
    strhs r2, [r12, #16]

    ldr r1, patch_exitcombo_arm7_block_address

    // Has the arm9 taken the request and asked us to get out of the way? This
    // comes first, whatever this side was doing: the arm9 is waiting with its
    // irqs off and only an answer from here lets it go on. A save since the
    // request went out, or one still settling, means not now: cancel, and the
    // request goes out again once the saves are quiet.
    ldr r2, [r1] // exit_request_block_t::requestMagic
    ldr r3, requestMagic
    cmp r2, r3
    ldrne r3, freezeRequestMagic
    cmpne r2, r3
    bne notTaken
    ldr r2, [r12, #8] // saveBusy
    ldr r3, [r12, #12] // savedSinceRequest
    orr r2, r2, r3
    ldr r3, [r12, #16] // saveCooldown
    orrs r2, r2, r3
    bne cancel
    ldr r2, [r1]
    ldr r3, requestMagic
    cmp r2, r3
    beq park
    b hold
cancel:
    ldr r2, parkMagic
    add r2, r2, #1 // EXIT_REQUEST_CANCEL_MAGIC
    str r2, [r1, #0x0C] // exit_request_block_t::comboMagic
    b dispatch

notTaken:
    // only armed when the loader stored a valid launcher path
    ldr r2, [r1, #4] // exit_request_block_t::pathMagic
    ldr r3, pathMagic
    cmp r2, r3
    bne dispatch

    ldr r2, [r12, #4] // requestFrames, 0 while there is no request
    cmp r2, #0
    beq noRequest

    // The request only goes out between saves, and the arm9 only takes it
    // between card operations. Give up after about five seconds without a gap,
    // and the combo has to be held again.
    add r2, r2, #1
    str r2, [r12, #4]
    cmp r2, #300
    bhs dropRequest

    ldr r2, [r1, #0x0C] // exit_request_block_t::comboMagic
    ldr r3, [r12, #20] // requestKind, the combo magic to publish
    cmp r2, r3
    bne notOut
    // out, but a save has started since: take it back until the saves are quiet
    ldr r2, [r12, #12] // savedSinceRequest
    cmp r2, #0
    movne r2, #0
    strne r2, [r1, #0x0C] // exit_request_block_t::comboMagic
    b dispatch

notOut:
    // send it out once no save has run for a while
    ldr r2, [r12, #8] // saveBusy
    ldr r3, [r12, #16] // saveCooldown
    orrs r2, r2, r3
    bne dispatch
    str r2, [r12, #12] // savedSinceRequest
    // The arm9 takes it on a vblank too. Published as both vblanks begin, it
    // could make this one or the next; a few lines in, it is always the next,
    // so the restore test pauses on the same frame each time.
    ldr r3, regKeyInput
    sub r3, r3, #0x128
    sub r3, r3, #2 // VCOUNT, 0x04000006
1:
    ldrh r2, [r3]
    cmp r2, #192
    blo 2f // a late irq, past the vblank lines: no race
    cmp r2, #200
    blo 1b
2:
    ldr r2, [r12, #20] // requestKind
    str r2, [r1, #0x0C] // exit_request_block_t::comboMagic
    b dispatch

dropRequest:
    mov r2, #0
    str r2, [r12, #4] // requestFrames
    str r2, [r12] // holdCounter
    str r2, [r1, #0x0C] // exit_request_block_t::comboMagic
    b dispatch

noRequest:
    // The rts menu's restore test: freeze by itself after rts.testFrames vblanks.
    ldr r2, [r1, #0x148] // rts.testFrames
    cmp r2, #0
    beq 1f
    subs r2, r2, #1
    str r2, [r1, #0x148]
    bne dispatch
    ldr r3, freezeComboMagic
    str r3, [r12, #20] // requestKind
    mov r2, #1
    str r2, [r12, #4] // requestFrames: out on the next quiet vblank
    b dispatch
1:
    // L+R+Select plus Right is the exit, plus Up the freeze test. Keys are
    // active low; the keycnt half of the word is never tested.
    ldr r2, regKeyInput
    ldr r2, [r2]
    ldr r3, comboCommonMask
    tst r2, r3
    bne notHeld
    tst r2, #0x10 // Right
    ldreq r3, comboMagic
    beq held
    tst r2, #0x40 // Up
    bne notHeld
    ldr r3, freezeComboMagic
held:
    // a different combo than last vblank starts the count again
    ldr r2, [r12, #20] // requestKind
    cmp r2, r3
    strne r3, [r12, #20]
    movne r2, #0
    strne r2, [r12] // holdCounter

    ldr r2, [r12]
    cmn r2, #1 // -1: held since the last freeze, wait for a release
    beq dispatch
    add r2, r2, #1
    str r2, [r12]
    cmp r2, #45
    blo dispatch

    // ask for the exit and carry on: quiescing happens once the arm9 answers,
    // so a game running without an arm9 patch is never left with a dead arm7
    mov r2, #1
    str r2, [r12, #4] // requestFrames, it goes out on a quiet vblank
    b dispatch

notHeld:
    mov r2, #0
    str r2, [r12]
    b dispatch

park:
    // Quiesce this cpu. Everything left out here is reinitialised by Pico Loader
    // itself on boot: irq enable/flags, the irq vector table, the fifo and the
    // individual sound channels. Stopping dma is the one thing nothing else does
    // in time, and the master sound register is silenced so the handover is quiet.
    mov r2, #0
    ldr r3, regIme
    str r2, [r3]
    ldr r3, regDmaBase
    str r2, [r3, #0x08] // DMA0CNT
    str r2, [r3, #0x14] // DMA1CNT
    str r2, [r3, #0x20] // DMA2CNT
    str r2, [r3, #0x2C] // DMA3CNT
    ldr r3, regSoundCnt
    strh r2, [r3]

    // Wait from arm7 wram, never from main memory. Pico Loader boots games with
    // main memory priority on the arm7 (EXMEMCNT bit 15), and a loop fetching its
    // opcodes from there asks again before the memory is free, so the arm9 would
    // never get another access and could not reload Pico Loader. The stub is in
    // main memory when the game leaves too little arm7 wram for the patch heap.
    // The game is never resumed, so its wram and r4-r12 are free to use.
    adr r0, parkWait
    ldmia r0, {r4-r12}
    ldr r0, parkWaitAddress
    stmia r0, {r4-r12}

    // Tell the arm9 we are out of the way, wait for it to finish reloading Pico
    // Loader, then release it and jump. The reboot code waits on the same three
    // step sync the sdk reset handler speaks, so it is unchanged. The arm9 stub
    // itself waits on PARK instead: it cannot rely on the sync the game left.
    ldr r3, regIpcSync
    mov r2, #1
    strb r2, [r3, #1] // sync out = 1, parked
    ldr r2, parkMagic
    str r2, [r1, #0x0C] // exit_request_block_t::comboMagic
    bx r0

// Runs from its copy in arm7 wram, so nothing pc relative. r3 = REG_IPC_SYNC.
parkWait:
1:
    ldrb r2, [r3]
    cmp r2, #1
    bne 1b // while ipc sync from arm9 is not 1

    mov r2, #0
    strb r2, [r3, #1] // sync out = 0, released

    // We were entered from an irq, so leave irq mode before handing over.
    // System mode specifically: Pico Loader's arm7 crt0 sets up only the stack
    // pointer of whatever mode it is entered in, and libtwl's thread switcher
    // saves and restores the user bank one, so it has to run as a system mode
    // thread. That is also the mode the other ways into the loader arrive in.
    msr cpsr_c, #0xDF // system mode, irqs off
    mov r0, #0x06000000 // vram c, mapped to the arm7 by the arm9 side
    ldr r0, [r0] // pload_header7_t::entryPoint
    bx r0
parkWaitEnd:
.if parkWaitEnd - parkWait != 9 * 4
.error "parkWait is copied as nine words"
.endif

// Freeze: r1 = block, r12 = holdCounter, r0 = 0, lr = which way in.
// The arm9 is in its wait piece with irqs off, its data cache written back.
// With the rts info from the loader and the save code's sd routines this runs
// the rts menu (step 2): stash RTS_STASH_SIZE bytes of main memory at
// rts.base, load the menu there and answer RUN, wait for the menu's answer and
// for it to have left, put the stash back, then CANCEL (resume) or park (exit).
// Without them it only holds for a second (step 1).
hold:
    stmfd sp!, {r4-r11, lr}
    mov r4, r1 // block, kept across the sd calls
    mov r5, r12 // state words
    // Say where the patch space is again: the loader's own entry for it has
    // been seen lost by the time a game runs.
    ldr r2, patch_exitcombo_arm7_space_address
    cmp r2, #0
    strne r2, [r4, #0x140] // rts.arm7PatchStart
    ldrne r2, patch_exitcombo_arm7_space_size
    strne r2, [r4, #0x144] // rts.arm7PatchSize
    ldr r3, regIme
    ldr r2, [r3]
    str r2, [r5, #24] // holdSavedIme
    mov r2, #0
    str r2, [r3] // IME = 0, the nested handler may have irqs on
    ldr r3, regSoundCnt
    ldrh r2, [r3]
    str r2, [r5, #28] // holdSavedSoundCnt
    mov r2, #0
    strh r2, [r3] // master sound off while frozen
    ldr r3, regIf
    ldr r2, [r3]
    str r2, [r5, #32] // holdSavedIf
    mov r0, #0
    bl shadow7Call // the timers it can start again stop

    ldr r2, [r4, #0x110] // rts.magic
    ldr r3, rtsInfoMagic
    cmp r2, r3
    bne timedHold
    ldr r2, patch_exitcombo_arm7_sdread_address
    ldr r3, patch_exitcombo_arm7_sdwrite_address
    cmp r2, #0
    cmpne r3, #0
    beq timedHold

    // What the arm9 sent and this cpu has not read yet: kept here, given back
    // as the menu leaves (A7FQ), or replaced by a loaded state's.
    mov r3, #0x04000000
    add r3, r3, #0x180
    ldrh r0, [r3, #4] // IPCFIFOCNT
    orr r1, r0, #0x8000 // enabled to read
    strh r1, [r3, #4]
    mov r2, #0
    add r6, r5, #76 // fifoWords
    mov r12, #0x04100000
9:
    ldrh r1, [r3, #4]
    tst r1, #0x100 // receive empty
    bne 9f
    ldr r1, [r12] // IPCFIFORECV
    str r1, [r6], #4
    add r2, r2, #1
    cmp r2, #16
    blo 9b
9:
    str r2, [r5, #72] // fifoCount
    ldr r1, fifoCntWritable
    and r0, r0, r1
    orr r0, r0, #0x4000 // and the error flag acknowledged
    strh r0, [r3, #4]

    ldr r0, [r4, #0x118] // rts.stashSector
    ldr r1, [r4, #0x114] // rts.base
    ldr r2, [r4, #0x11C] // rts.stashSectorCount
    ldr r3, patch_exitcombo_arm7_sdwrite_address
    bl callThumb
    ldr r0, [r4, #0x120] // rts.menuSector
    ldr r1, [r4, #0x114]
    ldr r2, [r4, #0x124] // rts.menuSectorCount
    ldr r3, patch_exitcombo_arm7_sdread_address
    bl callThumb

    mov r2, #0
    str r2, [r4, #0x128] // rts.menuState
    ldr r2, parkMagic
    add r2, r2, #2 // EXIT_REQUEST_RUN_MAGIC: the arm9 jumps to rts.base
    str r2, [r4, #0x0C]

    // Wait from arm7 wram (see park) for the menu's answer, then for it to
    // leave: it says LEFT with the last instructions it runs from the stash.
    adrl r1, menuWait
    bl copyWramWait
    mov r7, r0 // the wait loop in wram
    mov r2, #0
menuCommand:
    // r2 = the menu state last seen
    mov r1, r4
    mov lr, pc
    bx r7 // returns the new menu state in r0
    // Sector moves the menu asks for (save states), until it answers.
    ldr r3, menuStateSdWrite
    cmp r0, r3
    ldreq r3, patch_exitcombo_arm7_sdwrite_address
    beq 1f
    ldr r3, menuStateArm7Save
    cmp r0, r3
    beq arm7Save
    ldr r3, menuStateArm7Load
    cmp r0, r3
    beq arm7Load
    ldr r3, menuStateFifoQuery
    cmp r0, r3
    beq arm7FifoQuery
    ldr r3, menuStateFifoSend
    cmp r0, r3
    beq arm7FifoSend
    ldr r3, menuStateArm7Copy
    cmp r0, r3
    beq arm7Copy
    ldr r3, menuStateSdRead
    cmp r0, r3
    bne menuAnswered
    ldr r3, patch_exitcombo_arm7_sdread_address
1:
    ldr r0, [r4, #0x134] // rts.cmdSector
    ldr r1, [r4, #0x138] // rts.cmdAddress
    ldr r2, [r4, #0x13C] // rts.cmdSectorCount
    bl callThumb
    ldr r2, menuStateDone
    str r2, [r4, #0x128] // rts.menuState
    b menuCommand

// Save states (rtsmenu/state.h, arm7 context). Both run at command loop level:
// r4 = block, r5 = state words, r7 = the wait loop, sp as the menu's whole time.
arm7Save:
    ldr r0, [r4, #0x138] // rts.cmdAddress
    ldr r1, menuStateArm7Save
    mrs r2, spsr
    mov r3, sp
    stmia r0!, {r1, r2, r3} // magic, spsr_irq, sp_irq
    mrs r6, cpsr
    msr cpsr_c, #0xD3 // svc
    mrs r1, spsr
    mov r2, sp
    mov r3, lr
    stmia r0!, {r1, r2, r3}
    msr cpsr_c, #0xD7 // abt
    mrs r1, spsr
    mov r2, sp
    mov r3, lr
    stmia r0!, {r1, r2, r3}
    msr cpsr_c, #0xDB // und
    mrs r1, spsr
    mov r2, sp
    mov r3, lr
    stmia r0!, {r1, r2, r3}
    msr cpsr_c, #0xDF // sys
    mov r2, sp
    mov r3, lr
    stmia r0!, {r2, r3}
    msr cpsr_c, #0xD1 // fiq
    stmia r0!, {r8-r12}
    mov r1, sp
    mov r2, lr
    mrs r3, spsr
    stmia r0!, {r1, r2, r3}
    msr cpsr_c, r6
    ldr r3, regIf
    ldr r1, [r3, #-4] // IE
    str r1, [r0], #4
    ldr r1, [r5, #24] // holdSavedIme
    ldr r2, [r5, #28] // holdSavedSoundCnt
    ldr r3, [r5, #32] // holdSavedIf
    stmia r0!, {r1, r2, r3}
    mov r3, #0x04000000
    add r2, r3, #0x300
    ldrh r1, [r2, #4] // POWCNT2
    str r1, [r0], #4
    add r2, r3, #0x500
    ldrh r1, [r2, #4] // SOUNDBIAS
    str r1, [r0], #4
    add r2, r3, #0x100 // timers
    mov r6, #4
2:
    ldr r1, [r2], #4
    str r1, [r0], #4
    subs r6, r6, #1
    bne 2b
    add r2, r3, #0x400 // SOUNDxCNT
    mov r6, #16
3:
    ldr r1, [r2], #0x10
    str r1, [r0], #4
    subs r6, r6, #1
    bne 3b
    add r2, r5, #36 // holdStash: the game's wram words under the wait loop
    mov r6, #9
4:
    ldr r1, [r2], #4
    str r1, [r0], #4
    subs r6, r6, #1
    bne 4b
    adrl r1, patch_exitcombo_arm7_entry // which build of this stub, where
    str r1, [r0], #4
    // 0xE8: DISPSTAT | IPCSYNC << 16, IPCFIFOCNT | SPICNT << 16,
    // SNDCAP0CNT/SNDCAP1CNT, the four dma CNT_H in pairs, then their magic
    ldrh r1, [r3, #4] // r3 = 0x04000000
    add r2, r3, #0x180
    ldrh r2, [r2]
    orr r1, r1, r2, lsl #16
    add r2, r3, #0x180
    ldrh r6, [r2, #4]
    add r2, r3, #0x1C0
    ldrh r2, [r2]
    orr r2, r6, r2, lsl #16
    stmia r0!, {r1, r2}
    add r2, r3, #0x500
    ldrh r1, [r2, #8] // 0x04000508
    str r1, [r0], #4
    add r2, r3, #0xB8
    ldrh r1, [r2, #2] // DMA0CNT_H
    ldrh r6, [r2, #14] // DMA1CNT_H
    orr r1, r1, r6, lsl #16
    str r1, [r0], #4
    ldrh r1, [r2, #26]
    ldrh r6, [r2, #38]
    orr r1, r1, r6, lsl #16
    ldr r6, arm7CtxExtMagic
    stmia r0!, {r1, r6}
    b arm7Done

arm7Load:
    // Only a context from this same setup: same stub, same irq stack depth.
    ldr r0, [r4, #0x138] // rts.cmdAddress
    adrl r3, patch_exitcombo_arm7_entry
    ldr r1, [r0]
    ldr r2, menuStateArm7Save
    cmp r1, r2
    ldreq r1, [r0, #8] // sp_irq
    cmpeq r1, sp
    ldreq r1, [r0, #0xE4] // stub address
    cmpeq r1, r3
    ldrne r2, menuStateFail
    bne arm7Answer
    ldr r1, [r4, #0x13C] // rts.cmdSectorCount: 0 only checks
    cmp r1, #0
    beq arm7Done

    // Arm7 wram from the 64 KB after the context, all but the patch space,
    // whose code is the same and whose sd routines must keep their own state.
    // The frames above sp become the saved session's, which is what resumes.
    add r1, r0, #0x100
    mov r2, #0x03800000
    ldr r3, [r4, #0x140] // rts.arm7PatchStart
    ldr r12, [r4, #0x144] // rts.arm7PatchSize
    add r12, r3, r12
    add r8, r2, #0x10000 // r8-r11 are the hold's to use
5:
    cmp r2, r3
    cmphs r12, r2
    ldrls r6, [r1] // outside [start, end)
    strls r6, [r2]
    add r1, r1, #4
    add r2, r2, #4
    cmp r2, r8
    blo 5b

    // the context
    mrs r6, cpsr
    ldr r1, [r0, #4]
    msr spsr_cxsf, r1
    msr cpsr_c, #0xD3 // svc
    add r1, r0, #0x0C
    ldmia r1, {r1, r2, r3}
    msr spsr_cxsf, r1
    mov sp, r2
    mov lr, r3
    msr cpsr_c, #0xD7 // abt
    add r1, r0, #0x18
    ldmia r1, {r1, r2, r3}
    msr spsr_cxsf, r1
    mov sp, r2
    mov lr, r3
    msr cpsr_c, #0xDB // und
    add r1, r0, #0x24
    ldmia r1, {r1, r2, r3}
    msr spsr_cxsf, r1
    mov sp, r2
    mov lr, r3
    msr cpsr_c, #0xDF // sys
    ldr sp, [r0, #0x30]
    ldr lr, [r0, #0x34]
    msr cpsr_c, #0xD1 // fiq
    add r1, r0, #0x38
    ldmia r1, {r8-r12}
    ldr sp, [r0, #0x4C]
    ldr lr, [r0, #0x50]
    ldr r1, [r0, #0x54]
    msr spsr_cxsf, r1
    msr cpsr_c, r6

    ldr r3, regIf
    ldr r1, [r0, #0x58]
    str r1, [r3, #-4] // IE
    ldr r1, [r0, #0x5C]
    str r1, [r5, #24] // holdSavedIme, set again by holdEnd
    ldr r1, [r0, #0x60]
    str r1, [r5, #28] // holdSavedSoundCnt
    ldr r1, [r0, #0x64]
    str r1, [r5, #32] // holdSavedIf
    mov r3, #0x04000000
    add r2, r3, #0x300
    ldr r1, [r0, #0x68]
    strh r1, [r2, #4] // POWCNT2
    add r2, r3, #0x500
    ldr r1, [r0, #0x6C]
    strh r1, [r2, #4] // SOUNDBIAS
    // Timer controls (their reload values are write only), and every sound
    // channel stopped with the state's settings: the sample addresses are write
    // only too, so the game's driver starts its notes again.
    add r1, r0, #0x70
    add r2, r3, #0x100
    mov r6, #4
6:
    ldr r12, [r1], #4
    mov r12, r12, lsr #16
    strh r12, [r2, #2] // TMxCNT_H
    add r2, r2, #4
    subs r6, r6, #1
    bne 6b
    add r2, r3, #0x400
    mov r6, #16
    mov r8, #0
7:
    ldr r12, [r1], #4
    mov r8, r8, lsr #1
    tst r12, #0x80000000
    orrne r8, r8, #0x8000 // channel 15 - (r6 - 1) ends up at its own bit
    bic r12, r12, #0x80000000
    str r12, [r2], #0x10
    subs r6, r6, #1
    bne 7b
    orr r8, r8, #0x10000 // loaded: holdEnd starts them again from the shadow
    str r8, [r5, #140] // restartMask
    add r2, r5, #36 // holdStash, put back over the wait loop at the end
    mov r6, #9
8:
    ldr r12, [r1], #4
    str r12, [r2], #4
    subs r6, r6, #1
    bne 8b
    // From states that have them (0xFC: their magic).
    ldr r1, [r0, #0xFC]
    ldr r2, arm7CtxExtMagic
    cmp r1, r2
    bne arm7Done
    ldr r1, [r0, #0xE8]
    strh r1, [r3, #4] // DISPSTAT: vcount line and irq enables
    mov r1, r1, lsr #16
    and r1, r1, #0x4F00 // IPCSYNC: the output and the irq enable, never the send
    add r2, r3, #0x180
    strh r1, [r2]
    ldr r1, [r0, #0xEC]
    ldr r12, fifoCntWritable
    and r12, r1, r12
    orr r12, r12, #0x4000
    strh r12, [r2, #4] // IPCFIFOCNT
    // SPICNT: a transfer the running session left selected ends with one
    // dummy byte, then the state's settings (not its busy flag)
    add r2, r3, #0x1C0
    ldrh r12, [r2]
    tst r12, #0x800 // chip select hold
    beq 9f
    bic r12, r12, #0x800
    strh r12, [r2]
    mov r12, #0
    strh r12, [r2, #2] // SPIDATA
10:
    ldrh r12, [r2]
    tst r12, #0x80 // busy
    bne 10b
9:
    mov r1, r1, lsr #16
    bic r1, r1, #0x80
    strh r1, [r2]
    ldr r1, [r0, #0xF0]
    add r2, r3, #0x500
    strh r1, [r2, #8] // SNDCAP0CNT, SNDCAP1CNT
    // dma: the sources are write only, so only channels the state had off are
    // stopped; one it had on keeps what the running session set
    add r1, r0, #0xF4
    add r2, r3, #0xBA // DMA0CNT_H
    mov r6, #4
11:
    tst r6, #1
    ldreq r12, [r1], #4 // channels 0 and 2 start a word
    movne r12, r12, lsr #16
    tst r12, #0x8000
    moveq r8, #0
    strheq r8, [r2]
    add r2, r2, #12
    subs r6, r6, #1
    bne 11b
    b arm7Done

// Memory only this cpu sees, for the menu's checks: cmdSectorCount bytes (a
// multiple of 4) from the address in cmdSector to cmdAddress.
arm7Copy:
    ldr r0, [r4, #0x134]
    ldr r1, [r4, #0x138]
    ldr r6, [r4, #0x13C]
15:
    subs r6, r6, #4
    ldrpl r2, [r0], #4
    strpl r2, [r1], #4
    bhi 15b
    b arm7Done

// The arm7's kept fifo words to the menu: count, then 16 words.
arm7FifoQuery:
    ldr r0, [r4, #0x138] // rts.cmdAddress
    add r1, r5, #72
    mov r6, #17
12:
    ldr r2, [r1], #4
    str r2, [r0], #4
    subs r6, r6, #1
    bne 12b
    b arm7Done

// Words for the arm9's receive fifo: cmdSectorCount words at cmdAddress.
arm7FifoSend:
    ldr r0, [r4, #0x138]
    ldr r6, [r4, #0x13C]
    mov r3, #0x04000000
    add r3, r3, #0x180
    ldrh r12, [r3, #4]
    orr r1, r12, #0x8000 // enabled to send
    strh r1, [r3, #4]
    cmp r6, #16
    movhi r6, #16
13:
    subs r6, r6, #1
    bmi 14f
    ldrh r1, [r3, #4]
    tst r1, #2 // send full: the arm9 drained its side, so never
    bne 14f
    ldr r1, [r0], #4
    str r1, [r3, #8] // IPCFIFOSEND
    b 13b
14:
    ldr r1, fifoCntWritable
    and r12, r12, r1
    orr r12, r12, #0x4000
    strh r12, [r3, #4]
    b arm7Done
arm7Done:
    ldr r2, menuStateDone
arm7Answer:
    str r2, [r4, #0x128] // rts.menuState
    b menuCommand // r2 = what was answered

menuAnswered:
    mov r6, r0
    ldr r2, freezeComboMagic
    str r2, [r4, #0x0C] // off RUN: the menu may leave
    mov r1, r4
    mov r2, r6
    ldr r0, parkWaitAddress
    mov lr, pc
    bx r0 // until LEFT
    bl restoreWram

    ldr r0, [r4, #0x118]
    ldr r1, [r4, #0x114]
    ldr r2, [r4, #0x11C]
    ldr r3, patch_exitcombo_arm7_sdread_address
    bl callThumb // the game's main memory back

    mov r1, r4
    mov r12, r5
    ldr r2, menuStateExit
    cmp r6, r2
    // The rebooted loader goes to the launcher only for an EXIT request, and a
    // freeze left FREZ there: anything else counts as the game resetting itself.
    streq r6, [r1] // exit_request_block_t::requestMagic = EXIT, the same word
    beq park // r1 = block; never comes back, the stack does not matter
    b holdEnd

timedHold:
    // Count the second out from arm7 wram (see park). Polling only VCOUNT keeps
    // this cpu off main memory altogether.
    adrl r1, holdWait
    bl copyWramWait
    mov r2, #60 // frames
    mov r3, #0x04000000
    mov lr, pc
    bx r0
    bl restoreWram
    mov r1, r4
    mov r12, r5

holdEnd:
    // r1 = block, r12 = state words; r4-r11 are free until the ldmfd below
    mov r4, r1
    mov r5, r12
    ldr r1, [r12, #140] // restartMask, set by a load
    cmp r1, #0
    movne r0, #0
    strne r0, [r12, #140]
    movne r0, #8
    blne shadow7Call // the state's channels start again
    ldr r1, [r5, #32] // holdSavedIf: a load's are the state's
    mov r0, #4
    bl shadow7Call // the timers go on from their counts
    mov r1, r4
    mov r12, r5
    ldr r3, regSoundCnt
    ldr r2, [r12, #28] // holdSavedSoundCnt
    strh r2, [r3]
    // drop the vblank, hblank and vcount irqs that came in while frozen
    ldr r3, regIf
    ldr r2, [r3]
    ldr r0, [r12, #32] // holdSavedIf
    bic r2, r2, r0
    and r2, r2, #7
    str r2, [r3]

    mov r2, #0
    str r2, [r12, #4] // requestFrames
    mvn r2, #0
    str r2, [r12] // holdCounter = -1, the combo has to be released first
    ldr r2, parkMagic
    add r2, r2, #1 // EXIT_REQUEST_CANCEL_MAGIC: the arm9 goes on
    str r2, [r1, #0x0C] // exit_request_block_t::comboMagic
    ldr r3, regIme
    ldr r2, [r12, #24] // holdSavedIme
    str r2, [r3]
    ldmfd sp!, {r4-r11, lr}
    mov r0, #0 // the vblank index
    b dispatch

// Into the arm7 shadow's code (ShadowPatchCode7.s), version 3: r0 = 0 stops
// the timers whose reload it knows, 4 starts them again from their counts (r1
// = IF as frozen), 8 starts a loaded state's channels again (r1 = restartMask),
// 12 counts a vblank.
// Uses r0-r3, r12.
shadow7Call:
    ldr r2, patch_exitcombo_arm7_shadow7_address
    cmp r2, #0
    bxeq lr
    ldr r3, [r2]
    ldr r12, shadow7Magic
    cmp r3, r12
    bxne lr
    ldr r3, [r2, #4] // version
    cmp r3, #3
    bxne lr // this build's layout only
    add r3, r2, #0x170
    add r3, r3, r0
    bx r3

// Keeps the nine game words at parkWaitAddress and puts the nine word wait
// loop at r1 there instead. Returns r0 = parkWaitAddress. Uses r2, r3, r12.
copyWramWait:
    ldr r0, parkWaitAddress
    adrl r12, holdStash
    mov r3, #0
1:
    ldr r2, [r0, r3]
    str r2, [r12, r3]
    ldr r2, [r1, r3]
    str r2, [r0, r3]
    add r3, r3, #4
    cmp r3, #36
    bne 1b
    bx lr

// Puts the nine game words back at parkWaitAddress. Uses r0, r2, r3, r12.
restoreWram:
    ldr r0, parkWaitAddress
    adrl r12, holdStash
    mov r3, #0
1:
    ldr r2, [r12, r3]
    str r2, [r0, r3]
    add r3, r3, #4
    cmp r3, #36
    bne 1b
    bx lr

// From arm7 thumb code's point of view this is an ordinary call: r3 = the thumb
// routine, r0-r2 its arguments. The platform sd routines return with a thumb
// pop, so they must be called from thumb.
callThumb:
    adr r12, thumbCall + 1
    bx r12

// Copied to arm7 wram, so nothing pc relative. r1 = block, r2 = the value to
// wait to change. Returns the new rts.menuState in r0. The pause between reads
// keeps this off main memory, which the arm9 needs meanwhile.
menuWait:
1:
    mov r3, #0x400
2:
    subs r3, r3, #1
    bne 2b
    ldr r0, [r1, #0x128] // rts.menuState
    cmp r0, r2
    beq 1b
    bx lr
    .word 0, 0
menuWaitEnd:
.if menuWaitEnd - menuWait != 9 * 4
.error "menuWait is copied as nine words"
.endif

// Copied to arm7 wram, so nothing pc relative. r2 = frames, r3 = 0x04000000.
holdWait:
1:
    ldrh r0, [r3, #6] // VCOUNT
    cmp r0, #192
    bne 1b
2:
    ldrh r0, [r3, #6]
    cmp r0, #192
    beq 2b
    subs r2, r2, #1
    bne 1b
    bx lr
holdWaitEnd:
.if holdWaitEnd - holdWait != 9 * 4
.error "holdWait is copied as nine words"
.endif

dispatch:
    cmp lr, #0
    bne prioReturn

    // the dispatcher tail displaced by the hook
    ldr r1, patch_exitcombo_arm7_table_address
    ldr r0, [r1, r0, lsl #2]
    ldr lr, patch_exitcombo_arm7_return_address
    bx r0

prioReturn:
    ldmfd sp!, {r0}
prioDispatch:
    // the nested handler's dispatch displaced by the other hook: it keeps the
    // irq enable word and calls the handler. The handler is the irq vector for
    // as long as it runs, so the return point is that plus a fixed distance.
    mov r12, #0x04000000
    ldr r1, [r12, #0x210] // REG_IE
    stmfd sp!, {r1}
    ldr lr, regIrqVector
    ldr lr, [lr]
    ldr r1, patch_exitcombo_arm7_prioreturn_offset
    add lr, lr, r1
    bx r0

// The loader's save code reaches the sd card through these two instead of the
// platform routines, in the games that get the exit. They mark a save as running
// and as having happened since the request went out, and on the way back start
// the cooldown before clearing the running mark, so a vblank in between still
// sees the save. Thumb, because the platform routines return with a thumb pop.
.thumb
.global patch_exitcombo_arm7_sdread
.type patch_exitcombo_arm7_sdread, %function
patch_exitcombo_arm7_sdread:
    ldr r3, patch_exitcombo_arm7_sdread_address
    b saveCall

.global patch_exitcombo_arm7_sdwrite
.type patch_exitcombo_arm7_sdwrite, %function
patch_exitcombo_arm7_sdwrite:
    ldr r3, patch_exitcombo_arm7_sdwrite_address
saveCall:
    push {r4, r5, lr}
    adr r4, saveBusy
    movs r5, #1
    str r5, [r4] // saveBusy
    str r5, [r4, #4] // savedSinceRequest
    bl callR3
    adr r4, saveBusy
    movs r5, #10
    str r5, [r4, #8] // saveCooldown, a sixth of a second
    movs r5, #0
    str r5, [r4] // saveBusy
    pop {r4, r5}
    pop {r3}
    bx r3

callR3:
    bx r3

// Called from arm code by callThumb with lr = the arm return address.
thumbCall:
    push {lr}
    bl callR3
    pop {r3}
    bx r3

.balign 4

regKeyInput:
    .word 0x04000130

comboCommonMask:
    .word 0x0304 // L | R | Select

regIf:
    .word 0x04000214

regIme:
    .word 0x04000208

regDmaBase:
    .word 0x040000B0

regSoundCnt:
    .word 0x04000500

regIpcSync:
    .word 0x04000180

// The bottom of the game's arm7 stacks, right above its wram arena. Arm7Patcher
// only puts the patch heap in arm7 wram when it ends 0x2100 bytes below this.
parkWaitAddress:
    .word 0x0380F780

regIrqVector:
    .word 0x0380FFFC

requestMagic:
    .word 0x54495845 // EXIT_REQUEST_REQUEST_MAGIC

freezeRequestMagic:
    .word 0x555E4540 // EXIT_REQUEST_FREEZE_REQUEST_MAGIC

freezeComboMagic:
    .word 0x435A5246 // EXIT_REQUEST_FREEZE_COMBO_MAGIC

rtsInfoMagic:
    .word 0x31535452 // RTS_INFO_MAGIC

menuStateExit:
    .word 0x54495845 // RTS_MENU_STATE_EXIT

menuStateSdRead:
    .word 0x44524453 // RTS_MENU_STATE_SDREAD

menuStateSdWrite:
    .word 0x52574453 // RTS_MENU_STATE_SDWRITE

menuStateDone:
    .word 0x454E4F44 // RTS_MENU_STATE_DONE

menuStateFail:
    .word 0x4C494146 // RTS_MENU_STATE_FAIL

menuStateArm7Save:
    .word 0x58433741 // RTS_MENU_STATE_ARM7_SAVE

menuStateArm7Load:
    .word 0x444C3741 // RTS_MENU_STATE_ARM7_LOAD

menuStateFifoQuery:
    .word 0x51463741 // RTS_MENU_STATE_FIFO_QUERY 'A7FQ'

menuStateFifoSend:
    .word 0x53463741 // RTS_MENU_STATE_FIFO_SEND 'A7FS'

menuStateArm7Copy:
    .word 0x50433741 // RTS_MENU_STATE_ARM7_COPY 'A7CP'

arm7CtxExtMagic:
    .word 0x33585452 // 'RTX3': the context has the registers at 0xE8

fifoCntWritable:
    .word 0x8404 // IPCFIFOCNT: enable, receive and send irq enables

pathMagic:
    .word 0x58454C50 // EXIT_REQUEST_PATH_MAGIC

comboMagic:
    .word 0x424D4F43 // EXIT_REQUEST_COMBO_MAGIC

parkMagic:
    .word 0x4B524150 // EXIT_REQUEST_PARK_MAGIC

holdCounter:
    .word 0

requestFrames:
    .word 0

saveBusy:
    .word 0

savedSinceRequest:
    .word 0

saveCooldown:
    .word 0

requestKind: // holdCounter + 20
    .word 0

holdSavedIme: // holdCounter + 24
    .word 0

holdSavedSoundCnt: // + 28
    .word 0

holdSavedIf: // + 32
    .word 0

holdStash: // + 36, nine words
    .space 36

fifoCount: // + 72
    .word 0

fifoWords: // + 76, sixteen words
    .space 64

restartMask: // + 140: a load's channels to start again (soundRestart)
    .word 0

shadow7Magic:
    .word 0x37444853 // 'SHD7'

.global patch_exitcombo_arm7_shadow7_address
patch_exitcombo_arm7_shadow7_address:
    .word 0

.global patch_exitcombo_arm7_block_address
patch_exitcombo_arm7_block_address:
    .word 0

.global patch_exitcombo_arm7_table_address
patch_exitcombo_arm7_table_address:
    .word 0

.global patch_exitcombo_arm7_return_address
patch_exitcombo_arm7_return_address:
    .word 0

.global patch_exitcombo_arm7_prioreturn_offset
patch_exitcombo_arm7_prioreturn_offset:
    .word 0

.global patch_exitcombo_arm7_space_address
patch_exitcombo_arm7_space_address:
    .word 0

.global patch_exitcombo_arm7_space_size
patch_exitcombo_arm7_space_size:
    .word 0

.global patch_exitcombo_arm7_sdread_address
patch_exitcombo_arm7_sdread_address:
    .word 0

.global patch_exitcombo_arm7_sdwrite_address
patch_exitcombo_arm7_sdwrite_address:
    .word 0

.pool
.end
