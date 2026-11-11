// The parts of the TrustZone test that need exact control of registers: vector tables, secure
// gateways, and the calls whose register contents the test checks.
    .syntax unified
    .cpu cortex-m33
    .thumb

#define NS_STACK_TOP 0x20008000

// ------------------------------------------------------------------ Secure vectors and image block
    .section .s_vectors, "ax"
s_vectors:
    .word 0x20040000
    .word s_reset + 1
    .word s_default + 1         // NMI
    .word s_fault + 1           // HardFault
    .word s_default + 1         // MemManage
    .word s_fault + 1           // BusFault
    .word s_default + 1         // UsageFault
    .word s_fault + 1           // SecureFault
    .word 0, 0, 0
    .word s_default + 1         // SVCall
    .word 0, 0
    .word s_default + 1         // PendSV
    .word s_systick + 1
    .rept 52
    .word s_irq + 1
    .endr

    // The IMAGE_DEF that lets the boot ROM launch this from RAM.
    .p2align 2
    .word 0xffffded3
    .byte 0x42, 0x01
    .hword 0x1021               // EXE, Secure, Arm, RP2350
    .byte 0x03, 0x02
    .hword 0
    .word s_vectors
    .byte 0xff
    .hword 3
    .byte 0
    .word 0
    .word 0xab123579

    .text
    .thumb_func
s_reset:
    ldr r0, =0x20040000
    msr msp, r0
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    movs r2, #0
2:  cmp r0, r1
    bhs 3f
    str r2, [r0], #4
    b 2b
3:  bl s_main
1:  b 1b

    .thumb_func
s_default:
    mrs r0, ipsr
    bl s_unexpected
1:  b 1b

// HardFaults, BusFaults and SecureFaults resume where the test asks, so they need the stacked frame,
// which is on whichever stack EXC_RETURN names.
    .thumb_func
s_fault:
    tst lr, #0x40
    beq 1f
    tst lr, #4
    ite eq
    mrseq r0, msp
    mrsne r0, psp
    b 2f
1:  tst lr, #4
    ite eq
    mrseq r0, msp_ns
    mrsne r0, psp_ns
2:  push {r4, lr}
    mov r1, lr
    bl s_on_fault
    pop {r4, lr}
    bx lr

    .thumb_func
s_systick:
    push {r4, lr}
    mov r0, lr
    bl s_on_systick
    pop {r4, lr}
    bx lr

    .thumb_func
s_irq:
    push {r4, lr}
    mrs r0, ipsr
    bl s_on_irq
    pop {r4, lr}
    bx lr

// Calls the Non-secure function at r0 with r1 as its argument, returning 0xdead0001 rather than its
// result if the recognisable values left in the callee-saved registers do not come back.
    .global call_ns
    .thumb_func
call_ns:
    push {r4-r11, lr}
    bic r12, r0, #1
    mov r0, r1
    ldr r4, =0x44444444
    ldr r5, =0x55555555
    ldr r6, =0x66666666
    ldr r7, =0x77777777
    ldr r8, =0x88888888
    ldr r9, =0x99999999
    ldr r10, =0xaaaaaaaa
    ldr r11, =0xbbbbbbbb
    movs r1, #0
    movs r2, #0
    movs r3, #0
    blxns r12
    ldr r1, =0x44444444
    cmp r4, r1
    bne 9f
    ldr r1, =0x55555555
    cmp r5, r1
    bne 9f
    ldr r1, =0x88888888
    cmp r8, r1
    bne 9f
    ldr r1, =0xbbbbbbbb
    cmp r11, r1
    bne 9f
    pop {r4-r11, pc}
9:  ldr r0, =0xdead0001
    pop {r4-r11, pc}

// Sets the NVIC pending bits r1 in word r0 with recognisable values in the callee-saved registers,
// returning 1 if they do not survive the Non-secure handler.
    .global pend_and_check
    .thumb_func
pend_and_check:
    push {r4-r11, lr}
    ldr r4, =0x14141414
    ldr r5, =0x15151515
    ldr r6, =0x16161616
    ldr r7, =0x17171717
    ldr r8, =0x18181818
    ldr r9, =0x19191919
    ldr r10, =0x1a1a1a1a
    ldr r11, =0x1b1b1b1b
    ldr r2, =0xe000e200
    str r1, [r2, r0, lsl #2]
    dsb
    isb
    nop
    nop
    movs r0, #0
    ldr r1, =0x14141414
    cmp r4, r1
    bne 9f
    ldr r1, =0x17171717
    cmp r7, r1
    bne 9f
    ldr r1, =0x1b1b1b1b
    cmp r11, r1
    bne 9f
    pop {r4-r11, pc}
9:  movs r0, #1
    pop {r4-r11, pc}

// Branches to r0 without leaving Secure state, which for Non-secure memory is an INVTRAN
// SecureFault; the handler resumes at the pop.
    .global s_bad_transition
    .thumb_func
s_bad_transition:
    push {lr}
    orr r0, r0, #1
    adr.w r1, 1f
    ldr r2, =resume_to
    str r1, [r2]
    bx r0
    .p2align 2
1:  pop {pc}

// Leaf accesses, so the BusFault handler can resume a refused one at its return address.
    .global s_store
    .thumb_func
s_store:
    str r1, [r0]
    bx lr

    .global s_load
    .thumb_func
s_load:
    ldr r0, [r0]
    bx lr

    .global s_add
    .thumb_func
s_add:
    adds r0, r0, r1
    bxns lr

    .thumb_func
s_ipsr:
    mrs r0, ipsr
    bxns lr

// ------------------------------------------------------------------ Secure gateways
    .section .nsc, "ax"
    .global sg_add
    .thumb_func
sg_add:
    sg
    b.w s_add

    .global sg_ipsr
    .thumb_func
sg_ipsr:
    sg
    b.w s_ipsr

// ------------------------------------------------------------------ Non-secure vectors and code
    .section .ns_vectors, "ax"
    .global ns_vectors
ns_vectors:
    .word NS_STACK_TOP
    .word 0
    .rept 9
    .word ns_default + 1
    .endr
    .word ns_default + 1        // SVCall
    .word 0, 0
    .word ns_pendsv + 1
    .word ns_default + 1        // SysTick
    .rept 28
    .word ns_irq + 1
    .endr
    .word ns_bell + 1           // SIO_IRQ_BELL_NS
    .rept 23
    .word ns_irq + 1
    .endr

    .section .ns_text, "ax"
    .thumb_func
ns_default:
    mrs r0, ipsr
    ldr r1, =ns_unexpected
    str r0, [r1]
1:  b 1b

    .thumb_func
ns_pendsv:
    ldr r0, =ns_pendsv_count
    ldr r1, [r0]
    adds r1, #1
    str r1, [r0]
    bx lr

// The handler for the interrupt Secure code pends, which records the OR of every register the
// exception must have cleared so none of the Secure state they held leaks.
    .thumb_func
ns_irq:
    orrs r0, r0, r1
    orrs r0, r0, r2
    orrs r0, r0, r3
    orrs r0, r0, r4
    orrs r0, r0, r5
    orrs r0, r0, r6
    orrs r0, r0, r7
    mov r1, r8
    orrs r0, r0, r1
    mov r1, r9
    orrs r0, r0, r1
    mov r1, r10
    orrs r0, r0, r1
    mov r1, r11
    orrs r0, r0, r1
    mov r1, r12
    orrs r0, r0, r1
    ldr r1, =ns_irq_regs
    str r0, [r1]
    mrs r0, ipsr
    ldr r1, =ns_irq_ipsr
    str r0, [r1]
    ldr r1, =ns_irq_lr
    str lr, [r1]
    // Secure code must get its own callee-saved registers back from the stack regardless.
    movs r4, #0
    movs r7, #0
    mov r11, r4
    bx lr

    .thumb_func
ns_bell:
    ldr r0, =0xd0000188
    ldr r1, [r0]
    ldr r2, =ns_bell_seen
    str r1, [r2]
    str r1, [r0, #4]
    bx lr

// Leaf functions, so a fault handler can resume a faulting access or branch at its return address.
    .global ns_load
    .thumb_func
ns_load:
    ldr r0, [r0]
    bx lr

    .global ns_store
    .thumb_func
ns_store:
    str r1, [r0]
    bx lr

    .global ns_jump
    .thumb_func
ns_jump:
    orr r0, r0, #1
    bx r0
