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

    // the state words below: hold counter, request age and the save state the
    // wrappers at the end keep; the save cooldown loses a frame every vblank
    adr r12, holdCounter
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

// RTS step 1 test: hold the game for about a second, then let it go on.
// r1 = block, r12 = holdCounter, r0 = 0, lr = which way in.
// The arm9 is waiting in its wait piece with irqs off. It sees nothing but the
// FRZC it reacted to until this side answers CANCEL, which sends it down the
// path that gives the game back its irqs.
hold:
    stmfd sp!, {r4-r11, lr}
    ldr r3, regIme
    ldr r2, [r3]
    str r2, [r12, #24] // holdSavedIme
    mov r2, #0
    str r2, [r3] // IME = 0, the nested handler may have irqs on
    ldr r3, regSoundCnt
    ldrh r2, [r3]
    str r2, [r12, #28] // holdSavedSoundCnt
    mov r2, #0
    strh r2, [r3] // master sound off while frozen
    ldr r3, regIf
    ldr r2, [r3]
    str r2, [r12, #32] // holdSavedIf

    // Count the second out from arm7 wram (see park), in the nine words under
    // the game's stacks, which are kept here and put back after. Polling only
    // VCOUNT keeps this cpu off main memory altogether.
    ldr r0, parkWaitAddress
    add r2, r12, #36 // holdStash
    ldmia r0, {r3-r11}
    stmia r2, {r3-r11}
    adr r2, holdWait
    ldmia r2, {r3-r11}
    stmia r0, {r3-r11}
    mov r2, #60 // frames
    mov r3, #0x04000000
    mov lr, pc
    bx r0

    adr r12, holdCounter
    ldr r0, parkWaitAddress
    add r2, r12, #36 // holdStash
    ldmia r2, {r3-r11}
    stmia r0, {r3-r11}

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

.global patch_exitcombo_arm7_sdread_address
patch_exitcombo_arm7_sdread_address:
    .word 0

.global patch_exitcombo_arm7_sdwrite_address
patch_exitcombo_arm7_sdwrite_address:
    .word 0

.pool
.end
