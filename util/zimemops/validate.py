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

"""Execute cross-model Zimemops functional, trap and interrupt checks."""

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("gem5", type=Path)
parser.add_argument("--binaries", type=Path, required=True)
parser.add_argument("--out", type=Path, required=True)
parser.add_argument("--bytes", type=int, default=64, choices=range(1, 257))
parser.add_argument("--pack", type=int, default=32, choices=[16, 32])
parser.add_argument("--buffers", type=int, default=2, choices=[1, 2, 4])
parser.add_argument("--vlen", type=int, default=256, choices=[128, 256, 512])
parser.add_argument("--no-coalesce", action="store_true")
parser.add_argument("--no-store-priority", action="store_true")
parser.add_argument("--store-priority", action="store_true")
parser.add_argument("--store-reserve", type=int, default=1, choices=[0, 1, 2])
parser.add_argument("--reference", action="store_true")
parser.add_argument("--quad-batch", action="store_true")
parser.add_argument("--pair-control", action="store_true")
parser.add_argument(
    "--scalar-threshold", type=int, default=0, choices=range(9)
)
parser.add_argument("--publish-period", type=int, default=0)
parser.add_argument(
    "--prefetch", choices=["none", "read", "all"], default="none"
)
parser.add_argument("--unroll", type=int, default=16, choices=range(1, 65))
parser.add_argument("--jobs", type=int, default=max(1, os.cpu_count() // 8))
parser.add_argument("--timeout", type=int, default=600)
args = parser.parse_args()
if args.jobs < 1 or args.timeout < 1:
    parser.error("--jobs and --timeout must be positive")
gem5 = args.gem5.resolve()
binaries = args.binaries.resolve()
cases = []
for cpu in ("o3", "atomic", "timing"):
    cases.append(
        (
            f"{cpu}-functional",
            ["se.py", "--cpu", cpu, str(binaries / "bench-tests"), "test"],
        )
    )
    for bits in (32, 64):
        options = ["--rv32"] if bits == 32 else []
        cases.append(
            (
                f"{cpu}-traps{bits}",
                [
                    "fs.py",
                    "--cpu",
                    cpu,
                    *options,
                    str(binaries / f"traps{bits}"),
                ],
            )
        )
        cases.append(
            (
                f"{cpu}-stop-traps{bits}",
                [
                    "fs.py",
                    "--cpu",
                    cpu,
                    *options,
                    "--publish-period",
                    # Half a cycle in the 1 GHz bare-metal harness.
                    "500",
                    str(binaries / f"traps{bits}"),
                ],
            )
        )
    cases.append(
        (
            f"{cpu}-disabled",
            [
                "fs.py",
                "--cpu",
                cpu,
                "--disable-zimemops",
                str(binaries / "disabled64"),
            ],
        )
    )
    cases.append(
        (
            f"{cpu}-interrupt",
            ["fs.py", "--cpu", cpu, str(binaries / "interrupt64")],
        )
    )

    for operation in ("mmove", "mset"):
        cases.append(
            (
                f"{cpu}-interrupt-{operation}",
                [
                    "fs.py",
                    "--cpu",
                    cpu,
                    str(binaries / f"interrupt-{operation}64"),
                ],
            )
        )


def run(case):
    name, arguments = case
    baremetal = arguments[0] == "fs.py"
    directory = args.out / name
    directory.mkdir(parents=True, exist_ok=True)
    arguments[0] = str(root / arguments[0])
    arguments[1:1] = [
        "--scalar-threshold",
        str(args.scalar_threshold),
        "--vlen",
        str(args.vlen),
        "--store-reserve",
        str(args.store_reserve),
        "--buffers",
        str(args.buffers),
        "--prefetch",
        args.prefetch,
    ]
    if args.reference:
        arguments[1:1] = ["--reference"]
    if args.quad_batch:
        arguments[1:1] = ["--quad-batch"]
    if args.pair_control:
        arguments[1:1] = ["--pair-control"]
    if baremetal and args.publish_period:
        arguments[1:1] = ["--publish-period", str(args.publish_period)]
    if args.store_priority:
        arguments[1:1] = ["--store-priority"]
    if args.no_store_priority:
        arguments[1:1] = ["--no-store-priority"]
    if args.no_coalesce:
        arguments[1:1] = ["--no-coalesce"]
    arguments[1:1] = ["--unroll", str(args.unroll), "--bytes", str(args.bytes)]
    if args.pack:
        arguments[1:1] = ["--pack", str(args.pack)]
    command = [
        str(gem5),
        "--listener-mode=off",
        "-d",
        str(directory),
        *arguments,
    ]
    (directory / "command.json").write_text(
        json.dumps(command, indent=2) + "\n"
    )
    with (directory / "run.log").open("w") as log:
        try:
            result = subprocess.run(
                command, stdout=log, stderr=log, timeout=args.timeout
            )
            returncode = result.returncode
        except subprocess.TimeoutExpired:
            log.write(f"\nValidation timed out after {args.timeout} seconds\n")
            returncode = 124
    text = (directory / "run.log").read_text()
    passed = returncode == 0
    if "functional" in name:
        passed = passed and bool(
            re.search(r"PASS: [1-9][0-9]* Zimemops/memcpy cases\n", text)
        )
    if ("interrupt" in name and args.publish_period) or "-stop-" in name:
        passed = passed and bool(
            re.search(r"Progress publications: [1-9][0-9]*\n", text)
        )
    return {
        "case": name,
        "passed": passed,
        "returncode": returncode,
        "log": str(directory / "run.log"),
    }


with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
    results = list(pool.map(run, cases))
(args.out / "validation.json").write_text(json.dumps(results, indent=2) + "\n")
for result in results:
    print(result, flush=True)
raise SystemExit(0 if all(r["passed"] for r in results) else 1)
