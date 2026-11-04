# Copyright (c) 2026 ByteDance
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""Build the standalone memcpy and Zimemops validation programs."""

import argparse
import concurrent.futures
import json
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cc", required=True, help="RISC-V Linux GCC executable")
parser.add_argument("--out", type=Path, required=True)
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parent
repo = root.parents[1]
common = [args.cc, "-static"]
commands = [
    [
        *common,
        "-O2",
        "-march=rv64gcv",
        "-mabi=lp64d",
        "-fno-tree-vectorize",
        "-fno-tree-loop-distribute-patterns",
        "-fno-builtin",
        "-I",
        str(repo / "include"),
        str(root / "bench.c"),
        str(root / "copy.S"),
        str(repo / "util/m5/src/abi/riscv/m5op.S"),
        "-o",
        str(args.out / "bench"),
    ]
]
# Keep extended correctness coverage in a separate binary so changes to
# selftest code do not move the benchmark's instruction/predictor addresses.
commands.append(
    [
        *commands[0][:-2],
        "-DZIMEMOPS_EXTENDED_TESTS",
        "-o",
        str(args.out / "bench-tests"),
    ]
)
for name, bits, source, define in (
    ("traps32", 32, "traps.S", "CHECK_INSTRET"),
    ("traps64", 64, "traps.S", "CHECK_INSTRET"),
    ("disabled64", 64, "traps.S", "ZIMEMOPS_DISABLED"),
    ("interrupt64", 64, "interrupt.S", "OP=0"),
    ("interrupt-mmove64", 64, "interrupt.S", "OP=3"),
    ("interrupt-mset64", 64, "interrupt.S", "OP=6"),
):
    abi = "ilp32" if bits == 32 else "lp64"
    commands.append(
        [
            *common,
            "-nostdlib",
            f"-march=rv{bits}im_zicsr",
            f"-mabi={abi}",
            f"-D{define}",
            "-Wl,-Ttext=0x80000000",
            "-Wl,--no-relax",
            str(root / source),
            "-o",
            str(args.out / name),
        ]
    )
(args.out / "build-commands.json").write_text(
    json.dumps(commands, indent=2) + "\n"
)
with concurrent.futures.ThreadPoolExecutor(len(commands)) as pool:
    list(
        pool.map(lambda command: subprocess.run(command, check=True), commands)
    )
print(f"Built {len(commands)} programs in {args.out}")
