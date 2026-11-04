.cpu arm946e-s
.syntax unified
.section "shadowovlhook", "ax"
.thumb

// From the FS_StartOverlay hook (FsStartOverlayHookPatchAsm.s): r5 = the overlay's
// table entry, just loaded, not yet flushed. Returns the next patch in r0.
.global shadowovlhook_entry
.type shadowovlhook_entry, %function
shadowovlhook_entry:
    push {lr}
    ldr r1, shadowovlhook_target
    cmp r1, #0
    beq 1f
    movs r0, r5
    blx r1
1:
    ldr r0, shadowovlhook_next
    pop {pc}

.balign 4
.global shadowovlhook_target
shadowovlhook_target:
    .word 0
.global shadowovlhook_next
shadowovlhook_next:
    .word 0

.end
