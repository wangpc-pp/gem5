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
#ifndef OP
#define OP 0
#endif
.option norvc
.option norelax
.text
.globl _start
_start:
    la t0, trap
    csrw mtvec, t0
    li gp, 1
    la a0, destination
#if OP == 6
    li a1, 0x12343a
#else
    la a1, source
#endif
    li a2, 262144
    mv s0, a0
    mv s1, a1
    mv s2, a2
    li s4, 0
    li t0, 0x200bff8
    ld t1, 0(t0)
    addi t1, t1, 200
    li t0, 0x2004000
    sd t1, 0(t0)
    li t0, 0x80
    csrw mie, t0
    csrsi mstatus, 8
    la s5, transfer
    csrr s7, minstret
transfer:
    .insn r 0x0f, 3, OP, a0, a1, a2
    li gp, 2
    beqz s4, fail
    bnez a2, fail
#if OP == 3
    bne a0, s0, fail
    bne a1, s1, fail
#else
    add t0, s0, s2
    bne a0, t0, fail
#if OP == 6
    bne a1, s1, fail
#else
    add t0, s1, s2
    bne a1, t0, fail
#endif
#endif
    mv t0, s0
    li t1, 262144
    li t2, 0x3a
check_all:
    lbu t3, 0(t0)
    bne t3, t2, fail
    addi t0, t0, 1
    addi t1, t1, -1
    bnez t1, check_all
    li gp, 0
    j fail
.balign 4
trap:
    csrr t5, minstret
    li gp, 3
    csrr t0, mcause
    li t1, 1
    slli t1, t1, 63
    ori t1, t1, 7
    bne t0, t1, fail
    csrr t0, mepc
    bne t0, s5, fail
    li gp, 4
    sub t5, t5, s7
    li t0, 1
    bne t5, t0, fail
    beqz a2, fail
    bgeu a2, s2, fail
    sub t0, s2, a2
#if OP == 3
    bne a0, s0, fail
    bne a1, s1, fail
#else
    add t1, s0, t0
    bne a0, t1, fail
#if OP == 6
    bne a1, s1, fail
#else
    add t1, s1, t0
    bne a1, t1, fail
#endif
#endif
    li gp, 5
#if OP == 3
    add t1, s0, a2
    lbu t1, -1(t1)
#else
    lbu t1, 0(a0)
#endif
    li t2, 0x5a
    bne t1, t2, fail
#if OP == 3
    add t1, s0, a2
#else
    mv t1, s0
#endif
    li t2, 0x3a
check_prefix:
    lbu t3, 0(t1)
    bne t3, t2, fail
    addi t1, t1, 1
    addi t0, t0, -1
    bnez t0, check_prefix
    li t0, 0x2004000
    li t1, -1
    sd t1, 0(t0)
    csrw mie, zero
    li s4, 1
    mret
fail:
    li a0, 0
#if __riscv_xlen == 32
    li a1, 0
    mv a2, gp
    li a3, 0
#else
    mv a1, gp
#endif
    .long 0x4400007b
    j .
.data
.balign 4096
source:
    .fill 262144, 1, 0x3a
destination:
    .fill 262144, 1, 0x5a
