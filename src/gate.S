/* SPDX-License-Identifier: MIT
 * ARM32 register- and NEON-preserving trampoline gate.
 * Adapted from zsoltf/mpc-3-ce (controllers/program-observer/gate.S, MIT).
 */
.syntax unified
.arm
.fpu neon
.section .rodata
.balign 4
.global cb_gate_begin, cb_gate_end, cb_gate_handler, cb_gate_id
.hidden cb_gate_begin, cb_gate_end, cb_gate_handler, cb_gate_id

cb_gate_begin:
    push    {r0-r12, lr}
    mrs     r0, apsr
    vmrs    r1, fpscr
    push    {r0, r1}
    vpush   {d16-d31}
    vpush   {d0-d15}
    mov     r4, sp
    bic     sp, sp, #7
    mov     r0, r4
    ldr     r1, cb_gate_id
    ldr     ip, cb_gate_handler
    blx     ip
    mov     sp, r4
    vpop    {d0-d15}
    vpop    {d16-d31}
    pop     {r0, r1}
    vmsr    fpscr, r1
    msr     APSR_nzcvqg, r0
    pop     {r0-r12, lr}
    b       cb_gate_end
cb_gate_handler:
    .word   0
cb_gate_id:
    .word   0
cb_gate_end:

.section .note.GNU-stack,"",%progbits
