/* SPDX-License-Identifier: MIT
 * AArch64 register-, NZCV-, and NEON-preserving trampoline gate.
 *
 * Entered via `ldr x16, #8; br x16; .quad <stub>` (IP0 / x16 scratch per
 * AAPCS64). Starts with `bti c` (hint 34, NOP on pre-v8.5 cores, valid BTI
 * landing pad for `br x16` on v8.5+ cores). Preserves x0..x30, original sp,
 * NZCV, FPCR/FPSR, and q0..q31 across bridge_hook(GateCtx *ctx, uint32_t id),
 * then branches to cb_gate_end where the copied 16-byte original prologue
 * (including any `paciasp` signed with original sp/x30) and return jump live.
 *
 * Stack frame (800 bytes, 16-byte aligned):
 *   [sp +   0 .. 247]  x0..x30 (r[0..30], +8 pad at 248)
 *   [sp + 256 .. 287]  nzcv, fpcr, fpsr (+8 pad at 280)
 *   [sp + 288 .. 799]  q0..q31 (512 bytes)
 */
.section .rodata
.balign 8
.global cb_gate_begin, cb_gate_end, cb_gate_handler, cb_gate_id
.hidden cb_gate_begin, cb_gate_end, cb_gate_handler, cb_gate_id

cb_gate_begin:
    /* `bti c` encoded as hint #34 so older assemblers accept it without +bti */
    .inst   0xd503245f
    sub     sp, sp, #800

    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    stp     x30, xzr, [sp, #240]

    mrs     x0, nzcv
    mrs     x1, fpcr
    mrs     x2, fpsr
    stp     x0, x1,  [sp, #256]
    stp     x2, xzr, [sp, #272]

    stp     q0,  q1,  [sp, #288]
    stp     q2,  q3,  [sp, #320]
    stp     q4,  q5,  [sp, #352]
    stp     q6,  q7,  [sp, #384]
    stp     q8,  q9,  [sp, #416]
    stp     q10, q11, [sp, #448]
    stp     q12, q13, [sp, #480]
    stp     q14, q15, [sp, #512]
    stp     q16, q17, [sp, #544]
    stp     q18, q19, [sp, #576]
    stp     q20, q21, [sp, #608]
    stp     q22, q23, [sp, #640]
    stp     q24, q25, [sp, #672]
    stp     q26, q27, [sp, #704]
    stp     q28, q29, [sp, #736]
    stp     q30, q31, [sp, #768]

    mov     x0, sp
    ldr     w1, cb_gate_id
    ldr     x16, cb_gate_handler
    blr     x16

    ldp     q0,  q1,  [sp, #288]
    ldp     q2,  q3,  [sp, #320]
    ldp     q4,  q5,  [sp, #352]
    ldp     q6,  q7,  [sp, #384]
    ldp     q8,  q9,  [sp, #416]
    ldp     q10, q11, [sp, #448]
    ldp     q12, q13, [sp, #480]
    ldp     q14, q15, [sp, #512]
    ldp     q16, q17, [sp, #544]
    ldp     q18, q19, [sp, #576]
    ldp     q20, q21, [sp, #608]
    ldp     q22, q23, [sp, #640]
    ldp     q24, q25, [sp, #672]
    ldp     q26, q27, [sp, #704]
    ldp     q28, q29, [sp, #736]
    ldp     q30, q31, [sp, #768]

    ldp     x0, x1, [sp, #256]
    ldr     x2,     [sp, #272]
    msr     nzcv, x0
    msr     fpcr, x1
    msr     fpsr, x2

    ldp     x0,  x1,  [sp, #0]
    ldp     x2,  x3,  [sp, #16]
    ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30,      [sp, #240]

    add     sp, sp, #800
    b       cb_gate_end

.balign 8
cb_gate_handler:
    .quad   0
cb_gate_id:
    .word   0
    .word   0
cb_gate_end:

.section .note.GNU-stack,"",@progbits
