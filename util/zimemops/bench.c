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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gem5/m5ops.h>

typedef void *(*copy_fn)(void *, const void *, size_t);
extern void *copy_scalar(void *, const void *, size_t);
extern void *copy_rvv(void *, const void *, size_t);
extern void *copy_rvv_m1(void *, const void *, size_t);
extern void *copy_rvv_m2(void *, const void *, size_t);
extern void *copy_rvv_m4(void *, const void *, size_t);
extern void *copy_rvv_aligned_m1(void *, const void *, size_t);
extern void *copy_rvv_aligned_m2(void *, const void *, size_t);
extern void *copy_rvv_aligned_m4(void *, const void *, size_t);
extern void *copy_rvv_aligned_m8(void *, const void *, size_t);
extern void *copy_zimemops(void *, const void *, size_t);
extern void *move_zimemops(void *, const void *, size_t);
extern int check_zimemops_vector_state(void *, const void *, size_t, void *);

#define CAPACITY (2 * 1024 * 1024)
static unsigned char source[CAPACITY + 128] __attribute__((aligned(4096)));
static unsigned char target[CAPACITY + 128] __attribute__((aligned(4096)));
static unsigned char expected[8192];

static int
operation(int op, unsigned char *d, uintptr_t s, size_t n)
{
    uintptr_t dest = (uintptr_t)d, src = s, count = n;
    if (op == 0) {
        __asm__ volatile(".insn r 0x0f, 3, 0, %0, %1, %2"
                         : "+r"(dest), "+r"(src), "+r"(count)
                         :
                         : "memory");
    } else if (op == 1) {
        __asm__ volatile(".insn r 0x0f, 3, 3, %0, %1, %2"
                         : "+r"(dest), "+r"(src), "+r"(count)
                         :
                         : "memory");
    } else {
        __asm__ volatile(".insn r 0x0f, 3, 6, %0, %1, %2"
                         : "+r"(dest), "+r"(src), "+r"(count)
                         :
                         : "memory");
    }
    uintptr_t advance = op == 1 && (uintptr_t)d > s ? 0 : n;
    return count != 0 || dest != (uintptr_t)d + advance ||
           src != s + (op == 2 ? 0 : advance);
}

static int
selftest(void)
{
#ifdef ZIMEMOPS_EXTENDED_TESTS
    const unsigned short_sizes = 66;
    const size_t sizes[] = {95,   96,   97,   127,  128,  255, 256,
                            1023, 1024, 1025, 4095, 4096, 4097};
#else
    const unsigned short_sizes = 0;
    const size_t sizes[] = {0,  1,  2,  7,   8,   15,  16,  31,
                            32, 63, 64, 127, 128, 255, 256, 4097};
#endif
    copy_fn copies[] = {copy_scalar,         copy_rvv,
                        copy_zimemops,       copy_rvv_m1,
                        copy_rvv_m2,         copy_rvv_m4,
                        copy_rvv_aligned_m1, copy_rvv_aligned_m2,
                        copy_rvv_aligned_m4, copy_rvv_aligned_m8};
    unsigned cases = 0;
    for (unsigned i = 0; i < 8192; ++i) {
        source[i] = (i * 37 + 11) & 255;
    }
    for (unsigned k = 0; k < short_sizes + sizeof(sizes) / sizeof(sizes[0]);
         ++k) {
        size_t n = k < short_sizes ? k : sizes[k - short_sizes];
        /* Retain guards around all short-copy and shifted-move ranges. */
#ifdef ZIMEMOPS_EXTENDED_TESTS
        const size_t extent = n <= 65 ? 512 : 8192;
#else
        const size_t extent = 8192;
#endif
        for (unsigned offset = 0; offset < 8; ++offset) {
            for (unsigned op = 0; op < 3; ++op) {
                memset(target, 0xa5, extent);
                memset(expected, 0xa5, extent);
                if (operation(op, target + 64 + offset,
                              op == 2 ? 0x1234ab : (uintptr_t)(source + 65),
                              n)) {
                    return 10 + op;
                }
                for (unsigned i = 0; i < n; ++i) {
                    expected[64 + offset + i] =
                        op == 2 ? 0xab : source[65 + i];
                }
                if (memcmp(target, expected, extent)) {
                    return 20 + op;
                }
                ++cases;
            }
            for (unsigned method = 0;
                 method < sizeof(copies) / sizeof(copies[0]); ++method) {
                memset(target, 0xa5, extent);
                memset(expected, 0xa5, extent);
                for (size_t i = 0; i < n; ++i) {
                    expected[64 + offset + i] = source[64 + offset + i];
                }
                if (copies[method](target + 64 + offset, source + 64 + offset,
                                   n) != target + 64 + offset ||
                    memcmp(target, expected, extent)) {
                    return 30 + method;
                }
                ++cases;
            }
        }
        const int shifts[] = {
#ifdef ZIMEMOPS_EXTENDED_TESTS
            -64, -63, -32, -16, -8, 8, 16, 32, 63, 64,
#endif
            -31, -7,  -1,  0,   1,  7, 31};
        for (unsigned k = 0; k < sizeof(shifts) / sizeof(shifts[0]); ++k) {
            memcpy(target, source, extent);
            memcpy(expected, source, extent);
            unsigned char *s = target + 128;
            unsigned char *d = s + shifts[k];
            memmove(expected + 128 + shifts[k], expected + 128, n);
            if (operation(1, d, (uintptr_t)s, n) ||
                memcmp(target, expected, extent)) {
                return 40;
            }
            ++cases;
        }
    }
#ifdef ZIMEMOPS_EXTENDED_TESTS
    /* Short commands may split cache lines, but must preserve page restart. */
    const size_t short_edges[] = {63, 4095};
    for (unsigned k = 0; k < sizeof(short_edges) / sizeof(short_edges[0]);
         ++k) {
        const size_t edge = short_edges[k];
        for (size_t n = 1; n <= 65; ++n) {
            for (unsigned op = 0; op < 3; op += 2) {
                memset(target, 0xa5, 8192);
                memset(expected, 0xa5, 8192);
                if (op == 2) {
                    memset(expected + edge, 0xab, n);
                } else {
                    memcpy(expected + edge, source + edge - 1, n);
                }
                if (operation(op, target + edge,
                              op == 2 ? 0xab : (uintptr_t)(source + edge - 1),
                              n) ||
                    memcmp(target, expected, 8192)) {
                    return 54;
                }
                ++cases;
            }
            for (int shift = -1; shift <= 1; shift += 2) {
                memcpy(target, source, 8192);
                memcpy(expected, source, 8192);
                memmove(expected + edge + shift, expected + edge, n);
                if (operation(1, target + edge + shift,
                              (uintptr_t)(target + edge), n) ||
                    memcmp(target, expected, 8192)) {
                    return 55;
                }
                ++cases;
            }
        }
    }
    /* Active two/three/four-command bodies may cross a page between beats.
     * In particular, a 65-byte tail at page-64 must keep its last byte. */
    const size_t native_edges[] = {1, 31, 63, 4031, 4032, 4063, 4064};
    const size_t native_sizes[] = {33, 64,  65,  66,  95,  96,
                                   97, 128, 129, 159, 160, 161};
    for (unsigned k = 0; k < sizeof(native_edges) / sizeof(native_edges[0]);
         ++k) {
        const size_t edge = native_edges[k];
        for (unsigned j = 0;
             j < sizeof(native_sizes) / sizeof(native_sizes[0]); ++j) {
            const size_t n = native_sizes[j];
            for (unsigned op = 0; op < 3; ++op) {
                memset(target, 0xa5, 8192);
                memset(expected, 0xa5, 8192);
                if (op == 2) {
                    memset(expected + edge, 0xab, n);
                } else {
                    memcpy(expected + edge, source + edge - 1, n);
                }
                if (operation(op, target + edge,
                              op == 2 ? 0xab : (uintptr_t)(source + edge - 1),
                              n) ||
                    memcmp(target, expected, 8192)) {
                    return 56;
                }
                ++cases;
            }
        }
    }
    /* Long-copy alignment heads may use one GPR before the vector loop.
     * Check exact 1-8-byte heads with differing source alignments. */
    const size_t long_head_sizes[] = {4096, 4097};
    for (unsigned k = 0;
         k < sizeof(long_head_sizes) / sizeof(long_head_sizes[0]); ++k) {
        const size_t n = long_head_sizes[k];
        for (unsigned offset = 56; offset < 64; ++offset) {
            for (unsigned src_offset = 0; src_offset < 2; ++src_offset) {
                memset(target, 0xa5, 8192);
                memset(expected, 0xa5, 8192);
                memcpy(expected + offset, source + src_offset, n);
                if (operation(0, target + offset,
                              (uintptr_t)(source + src_offset), n) ||
                    memcmp(target, expected, 8192)) {
                    return 57;
                }
                ++cases;
            }
            memset(target, 0xa5, 8192);
            memset(expected, 0xa5, 8192);
            memset(expected + offset, 0xab, n);
            if (operation(2, target + offset, 0xab, n) ||
                memcmp(target, expected, 8192)) {
                return 58;
            }
            ++cases;
        }
    }
#endif
    for (unsigned op = 0; op < 3; ++op) {
        if (operation(op, (void *)(uintptr_t)-1, (uintptr_t)-2, 0)) {
            return 50;
        }
        ++cases;
    }
    /* Peeled kernels must also handle different source/destination offsets. */
    const size_t edges[] = {0, 1, 31, 64, 4097};
    for (unsigned k = 0; k < sizeof(edges) / sizeof(edges[0]); ++k) {
        for (unsigned src_offset = 0; src_offset < 2; ++src_offset) {
            for (unsigned dst_offset = 0; dst_offset < 8; ++dst_offset) {
                for (unsigned method = 6; method < 10; ++method) {
                    memset(target, 0xa5, 8192);
                    memset(expected, 0xa5, 8192);
                    memcpy(expected + 64 + dst_offset,
                           source + 64 + src_offset, edges[k]);
                    if (copies[method](target + 64 + dst_offset,
                                       source + 64 + src_offset,
                                       edges[k]) != target + 64 + dst_offset ||
                        memcmp(target, expected, 8192)) {
                        return 60 + method;
                    }
                    ++cases;
                }
            }
        }
    }
    uintptr_t d = (uintptr_t)target, n = 13;
    __asm__ volatile(".insn r 0x0f, 3, 6, %0, zero, %1"
                     : "+r"(d), "+r"(n)
                     :
                     : "memory");
    if (d != (uintptr_t)target + 13 || n) {
        return 51;
    }
    for (unsigned i = 0; i < 13; ++i) {
        if (target[i]) {
            return 52;
        }
    }
    const size_t state_sizes[] = {
#ifdef ZIMEMOPS_EXTENDED_TESTS
        0,   1, 2, 3, 4, 7, 8, 9, 16, 31, 32, 63, 64, 65,
#endif
        4097};
    for (unsigned k = 0; k < sizeof(state_sizes) / sizeof(state_sizes[0]);
         ++k) {
        if (check_zimemops_vector_state(target, source, state_sizes[k],
                                        expected)) {
            return 53;
        }
        for (unsigned i = 0; i < 7; ++i) {
            if (expected[i] != 0x39) {
                return 54;
            }
        }
        ++cases;
    }
    printf("PASS: %u Zimemops/memcpy cases\n", cases + 1);
    return 0;
}

static int
overlap_bench(size_t size, int shift, unsigned reps)
{
    if (size > CAPACITY / 2 || shift < -64 || shift > 64 || !reps) {
        return 2;
    }
    const size_t footprint = size + 128;
    for (size_t i = 0; i < footprint; ++i) {
        source[i] = target[i] = (i * 37 + 11) & 255;
    }
    for (unsigned i = 0; i < 2; ++i) {
        move_zimemops(target + 64 + shift, target + 64, size);
        memmove(source + 64 + shift, source + 64, size);
    }
    m5_work_begin(0, 0);
    for (unsigned i = 0; i < reps; ++i) {
        move_zimemops(target + 64 + shift, target + 64, size);
    }
    __asm__ volatile("fence rw, rw" : : : "memory");
    m5_work_end(0, 0);
    for (unsigned i = 0; i < reps; ++i) {
        memmove(source + 64 + shift, source + 64, size);
    }
    if (memcmp(source, target, footprint)) {
        return 1;
    }
    printf("PASS: mmove size=%zu shift=%d reps=%u\n", size, shift, reps);
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "test")) {
        return selftest();
    }
    if (argc != 6) {
        fprintf(stderr,
                "usage: %s method size offset warm|stream|overlap reps\n",
                argv[0]);
        return 2;
    }
    copy_fn fn = !strcmp(argv[1], "scalar")         ? copy_scalar
                 : !strcmp(argv[1], "rvv")          ? copy_rvv
                 : !strcmp(argv[1], "rvv-m1")       ? copy_rvv_m1
                 : !strcmp(argv[1], "rvv-m2")       ? copy_rvv_m2
                 : !strcmp(argv[1], "rvv-m4")       ? copy_rvv_m4
                 : !strcmp(argv[1], "rvv-align-m1") ? copy_rvv_aligned_m1
                 : !strcmp(argv[1], "rvv-align-m2") ? copy_rvv_aligned_m2
                 : !strcmp(argv[1], "rvv-align-m4") ? copy_rvv_aligned_m4
                 : !strcmp(argv[1], "rvv-align-m8") ? copy_rvv_aligned_m8
                 : !strcmp(argv[1], "zimemops")     ? copy_zimemops
                                                    : NULL;
    size_t size = strtoul(argv[2], NULL, 0);
    size_t offset = strtoul(argv[3], NULL, 0);
    unsigned reps = strtoul(argv[5], NULL, 0);
    if (!strcmp(argv[4], "overlap")) {
        if (strcmp(argv[1], "zimemops")) {
            return 2;
        }
        return overlap_bench(size, strtol(argv[3], NULL, 0), reps);
    }
    int stream = !strcmp(argv[4], "stream");
    if (!fn || size > CAPACITY / 2 || offset > 63 || !reps) {
        return 2;
    }
    size_t stride = (size + 127) & ~(size_t)63;
    size_t slots = stream ? CAPACITY / stride : 1;
    size_t footprint = stream ? CAPACITY : stride;
    memset(source, 0x5b, footprint + 128);
    memset(target, 0xa5, footprint + 128);
    if (!stream) {
        fn(target + offset, source + offset, size);
        fn(target + offset, source + offset, size);
    }
    m5_work_begin(0, 0);
    if (stream) {
        size_t pos = offset;
        const size_t end = slots * stride + offset;
        for (unsigned r = 0; r < reps; ++r) {
            fn(target + pos, source + pos, size);
            pos += stride;
            if (pos == end) {
                pos = offset;
            }
        }
    } else {
        // A runtime remainder here would consume a non-pipelined divider
        // every copy, even for slots == 1, hiding short-kernel improvements.
        for (unsigned r = 0; r < reps; ++r) {
            fn(target + offset, source + offset, size);
        }
    }
    __asm__ volatile("fence rw, rw" : : : "memory");
    m5_work_end(0, 0);
    size_t visited = reps < slots ? reps : slots;
    uint64_t checksum = 0;
    for (size_t r = 0; r < visited; ++r) {
        size_t pos = r * stride + offset;
        if (memcmp(target + pos, source + pos, size)) {
            return 1;
        }
        if (size) {
            checksum += target[pos] + target[pos + size - 1];
        }
        if (offset && target[pos - 1] != 0xa5) {
            return 1;
        }
        if (target[pos + size] != 0xa5) {
            return 1;
        }
    }
    printf("PASS: %s size=%zu offset=%zu mode=%s reps=%u checksum=%llu\n",
           argv[1], size, offset, argv[4], reps, (unsigned long long)checksum);
    return 0;
}
