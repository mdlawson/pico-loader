#ifdef RTS_AUTOPILOT
.cpu arm946e-s
.syntax unified
.section "autopilothalt", "ax"
.arm

// The autopilot build's timing: each "wait for interrupt" (mcr p15, 0, rX, c7, c0, 4)
// in the game's arm9 branches here instead (one copy of this per place). It halts
// with irqs masked, so the time it adds is the halt alone: the cpu still wakes for a
// pending irq, and the handler runs as the mask comes off, as it would have. A thread
// switch in that handler is then no longer in the count. The scanlines halted (VCOUNT
// before and after, a frame wraps at 263) go to rts_autopilot_t::idle, in the shared
// page the arm9 reads and writes uncached.
.global autopilothalt_entry
.type autopilothalt_entry, %function
autopilothalt_entry:
    push {r0-r3}
    mrs r3, cpsr
    orr r0, r3, #0x80
    msr cpsr_c, r0
    mov r1, #0x04000000
    ldrh r0, [r1, #6] // VCOUNT
    mov r2, #0
    mcr p15, 0, r2, c7, c0, 4 // wait for interrupt
    ldrh r2, [r1, #6]
    subs r2, r2, r0
    addmi r2, r2, #256 // a frame is 263 lines
    addmi r2, r2, #7
    ldr r1, autopilothalt_idle
    ldr r0, [r1]
    add r0, r0, r2
    str r0, [r1]
    msr cpsr_c, r3
    pop {r0-r3}
.global autopilothalt_return
autopilothalt_return:
    b . // the installer puts b site + 4 here

.global autopilothalt_idle
autopilothalt_idle:
    .word 0

.end
#endif
