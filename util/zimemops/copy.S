/*
 * Copyright (c) 2026 ByteDance
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
.text
.align 4
.globl copy_scalar
.type copy_scalar, @function
copy_scalar:
    mv t0, a0
    xor t1, a0, a1
    andi t1, t1, 7
    bnez t1, .Lbytes
.Lalign:
    beqz a2, .Lreturn
    andi t1, t0, 7
    beqz t1, .Lbulk
    lbu t1, 0(a1)
    sb t1, 0(t0)
    addi a1, a1, 1
    addi t0, t0, 1
    addi a2, a2, -1
    j .Lalign
.Lbulk:
    li t1, 32
    bltu a2, t1, .Lwords
.Lbulk_loop:
    ld t2, 0(a1)
    ld t3, 8(a1)
    ld t4, 16(a1)
    ld t5, 24(a1)
    sd t2, 0(t0)
    sd t3, 8(t0)
    sd t4, 16(t0)
    sd t5, 24(t0)
    addi a1, a1, 32
    addi t0, t0, 32
    addi a2, a2, -32
    bgeu a2, t1, .Lbulk_loop
.Lwords:
    li t1, 8
    bltu a2, t1, .Lbytes
.Lword_loop:
    ld t2, 0(a1)
    sd t2, 0(t0)
    addi a1, a1, 8
    addi t0, t0, 8
    addi a2, a2, -8
    bgeu a2, t1, .Lword_loop
.Lbytes:
    beqz a2, .Lreturn
.Lbyte_loop:
    lbu t1, 0(a1)
    sb t1, 0(t0)
    addi a1, a1, 1
    addi t0, t0, 1
    addi a2, a2, -1
    bnez a2, .Lbyte_loop
.Lreturn:
    ret
.size copy_scalar, .-copy_scalar

.align 4
.globl copy_rvv
.type copy_rvv, @function
copy_rvv:
    mv t0, a0
    beqz a2, .Lrvv_done
.Lrvv_loop:
    vsetvli t1, a2, e8, m8, ta, ma
    vle8.v v8, (a1)
    vse8.v v8, (t0)
    add a1, a1, t1
    add t0, t0, t1
    sub a2, a2, t1
    bnez a2, .Lrvv_loop
.Lrvv_done:
    ret
.size copy_rvv, .-copy_rvv

.align 4
.globl copy_zimemops
.type copy_zimemops, @function
copy_zimemops:
    mv t0, a0
    /* mcopy already leaves memory and pointers untouched for a zero count. */
    .insn r 0x0f, 3, 0, t0, a1, a2
    ret
.size copy_zimemops, .-copy_zimemops

.align 4
.globl copy_rvv_m1
.type copy_rvv_m1, @function
copy_rvv_m1:
    mv t0, a0
    beqz a2, .Lrvv_m1_done
.Lrvv_m1_loop:
    vsetvli t1, a2, e8, m1, ta, ma
    vle8.v v8, (a1)
    vse8.v v8, (t0)
    add a1, a1, t1
    add t0, t0, t1
    sub a2, a2, t1
    bnez a2, .Lrvv_m1_loop
.Lrvv_m1_done:
    ret
.size copy_rvv_m1, .-copy_rvv_m1

.align 4
.globl copy_rvv_m2
.type copy_rvv_m2, @function
copy_rvv_m2:
    mv t0, a0
    beqz a2, .Lrvv_m2_done
.Lrvv_m2_loop:
    vsetvli t1, a2, e8, m2, ta, ma
    vle8.v v8, (a1)
    vse8.v v8, (t0)
    add a1, a1, t1
    add t0, t0, t1
    sub a2, a2, t1
    bnez a2, .Lrvv_m2_loop
.Lrvv_m2_done:
    ret
.size copy_rvv_m2, .-copy_rvv_m2

.align 4
.globl copy_rvv_m4
.type copy_rvv_m4, @function
copy_rvv_m4:
    mv t0, a0
    beqz a2, .Lrvv_m4_done
.Lrvv_m4_loop:
    vsetvli t1, a2, e8, m4, ta, ma
    vle8.v v8, (a1)
    vse8.v v8, (t0)
    add a1, a1, t1
    add t0, t0, t1
    sub a2, a2, t1
    bnez a2, .Lrvv_m4_loop
.Lrvv_m4_done:
    ret
.size copy_rvv_m4, .-copy_rvv_m4

.macro rvv_aligned name, lmul
.align 4
.globl \name
.type \name, @function
\name:
    mv t0, a0
    beqz a2, .Laligned_done_\@
    neg t2, t0
    andi t2, t2, 63
    beqz t2, .Laligned_bulk_\@
.Laligned_prefix_\@:
    bgeu a2, t2, .Laligned_size_\@
    mv t2, a2
.Laligned_size_\@:
    vsetvli t1, t2, e8, \lmul, ta, ma
    vle8.v v8, (a1)
    vse8.v v8, (t0)
    add a1, a1, t1
    add t0, t0, t1
    sub a2, a2, t1
    sub t2, t2, t1
    beqz a2, .Laligned_done_\@
    bnez t2, .Laligned_prefix_\@
.Laligned_bulk_\@:
    /* Constant VL avoids configuration barriers inside the bulk loop. */
    vsetvli t1, zero, e8, \lmul, ta, ma
    slli t3, t1, 2
    bltu a2, t3, .Laligned_single_\@
.Laligned_unrolled_\@:
    .rept 4
    vle8.v v8, (a1)
    vse8.v v8, (t0)
    add a1, a1, t1
    add t0, t0, t1
    .endr
    sub a2, a2, t3
    bgeu a2, t3, .Laligned_unrolled_\@
.Laligned_single_\@:
    bltu a2, t1, .Laligned_tail_\@
.Laligned_loop_\@:
    vle8.v v8, (a1)
    vse8.v v8, (t0)
    add a1, a1, t1
    add t0, t0, t1
    sub a2, a2, t1
    bgeu a2, t1, .Laligned_loop_\@
.Laligned_tail_\@:
    beqz a2, .Laligned_done_\@
    vsetvli t1, a2, e8, \lmul, ta, ma
    vle8.v v8, (a1)
    vse8.v v8, (t0)
.Laligned_done_\@:
    ret
.size \name, .-\name
.endm

/* Keep unpeeled kernels too: the best RVV envelope includes both choices. */
rvv_aligned copy_rvv_aligned_m1, m1
rvv_aligned copy_rvv_aligned_m2, m2
rvv_aligned copy_rvv_aligned_m4, m4
rvv_aligned copy_rvv_aligned_m8, m8

/* Zimemops uses private vector-class temporaries, not architectural V state. */
.align 4
.globl check_zimemops_vector_state
.type check_zimemops_vector_state, @function
check_zimemops_vector_state:
    li t0, 7
    vsetvli t1, t0, e8, m1, tu, mu
    li t0, 0x39
    vmv.v.x v31, t0
    csrr t2, vtype
    csrr t3, vl
    .insn r 0x0f, 3, 0, a0, a1, a2
    csrr t4, vtype
    bne t2, t4, .Lstate_fail
    csrr t4, vl
    bne t3, t4, .Lstate_fail
    csrr t4, vstart
    bnez t4, .Lstate_fail
    vse8.v v31, (a3)
    li a0, 0
    ret
.Lstate_fail:
    li a0, 1
    ret
.size check_zimemops_vector_state, .-check_zimemops_vector_state

/* Separate wrapper keeps overlap measurements out of memcpy kernels. */
.align 4
.globl move_zimemops
.type move_zimemops, @function
move_zimemops:
    mv t0, a0
    .insn r 0x0f, 3, 3, t0, a1, a2
    ret
.size move_zimemops, .-move_zimemops

.section .note.GNU-stack,"",@progbits
