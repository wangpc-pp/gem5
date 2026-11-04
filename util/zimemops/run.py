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

"""Run reproducible memcpy ROIs and retain every gem5 log/config/stat file."""

import argparse
import concurrent.futures
import csv
import hashlib
import json
import os
import subprocess
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--simulator-build",
        type=Path,
        help="Recorded simulator build metadata",
    )
    parser.add_argument("--bytes", type=int, default=64, choices=range(1, 257))
    parser.add_argument("--pack", type=int, default=32, choices=[16, 32])
    parser.add_argument("--buffers", type=int, default=2, choices=[1, 2, 4])
    parser.add_argument(
        "--vlen", type=int, default=256, choices=[128, 256, 512]
    )
    parser.add_argument("--no-coalesce", action="store_true")
    parser.add_argument("--no-store-priority", action="store_true")
    parser.add_argument("--store-priority", action="store_true")
    parser.add_argument(
        "--store-reserve", type=int, default=1, choices=[0, 1, 2]
    )
    parser.add_argument("--reference", action="store_true")
    parser.add_argument("--quad-batch", action="store_true")
    parser.add_argument("--pair-control", action="store_true")
    parser.add_argument(
        "--scalar-threshold", type=int, default=0, choices=range(9)
    )
    parser.add_argument(
        "--prefetch", choices=["none", "read", "all"], default="none"
    )
    parser.add_argument("--unroll", type=int, default=16, choices=range(1, 65))
    parser.add_argument(
        "--sizes",
        type=int,
        nargs="+",
        default=[0, 16, 64, 256, 1024, 4096, 65536],
    )
    parser.add_argument("--offsets", type=int, nargs="+", default=[0, 1])
    parser.add_argument(
        "--modes",
        nargs="+",
        choices=["warm", "stream", "overlap"],
        default=["warm", "stream"],
    )
    parser.add_argument(
        "--methods",
        nargs="+",
        choices=[
            "scalar",
            "rvv",
            "rvv-m1",
            "rvv-m2",
            "rvv-m4",
            "rvv-align-m1",
            "rvv-align-m2",
            "rvv-align-m4",
            "rvv-align-m8",
            "zimemops",
        ],
        default=[
            "scalar",
            "rvv",
            "rvv-m1",
            "rvv-m2",
            "rvv-m4",
            "rvv-align-m1",
            "rvv-align-m2",
            "rvv-align-m4",
            "rvv-align-m8",
            "zimemops",
        ],
    )
    parser.add_argument(
        "--jobs", type=int, default=max(1, os.cpu_count() // 4)
    )
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    if args.jobs < 1 or args.timeout < 1:
        parser.error("--jobs and --timeout must be positive")
    if any(size < 0 or size > 1024 * 1024 for size in args.sizes):
        parser.error("--sizes must be between 0 and 1048576")
    if "overlap" in args.modes and args.methods != ["zimemops"]:
        parser.error("overlap measures mmove; use --methods zimemops")
    for mode in args.modes:
        low, high = (-64, 64) if mode == "overlap" else (0, 63)
        if any(offset < low or offset > high for offset in args.offsets):
            parser.error(f"{mode} offsets must be between {low} and {high}")
    args.out.mkdir(parents=True, exist_ok=True)
    config = Path(__file__).with_name("se.py")
    repo = config.parents[2]
    revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=repo, text=True
    ).strip()
    patch = subprocess.check_output(["git", "diff", "HEAD"], cwd=repo)
    (args.out / "source-diff.patch").write_bytes(patch)
    with args.gem5.open("rb") as binary:
        digest = hashlib.sha256()
        for block in iter(lambda: binary.read(1024 * 1024), b""):
            digest.update(block)
    simulator_build = None
    if args.simulator_build:
        simulator_build = json.loads(args.simulator_build.read_text())
        if simulator_build["gem5_sha256"] != digest.hexdigest():
            parser.error("--simulator-build does not match the simulator hash")
    metadata_path = args.out / "metadata.json"
    config_digest = hashlib.sha256(config.read_bytes()).hexdigest()
    arguments = {
        k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()
    }
    if args.resume:
        if not metadata_path.exists():
            parser.error("--resume requires an existing metadata.json")
        previous = json.loads(metadata_path.read_text())
        previous_config = previous.get("configuration_sha256")
        if previous_config is None:
            # Older reports predate configuration hashes. Accept them only
            # on the recorded revision with an unchanged tracked config.
            clean_config = (
                subprocess.run(
                    ["git", "diff", "--quiet", "HEAD", "--", str(config)],
                    cwd=repo,
                ).returncode
                == 0
            )
            if previous["revision"] != revision or not clean_config:
                parser.error("--resume requires a recorded matching config")
            previous_config = config_digest
        ignored = {"jobs", "timeout", "resume"}
        old_args = {
            k: v for k, v in previous["arguments"].items() if k not in ignored
        }
        new_args = {k: v for k, v in arguments.items() if k not in ignored}
        if (
            previous["gem5_sha256"] != digest.hexdigest()
            or previous["workload_sha256"]
            != hashlib.sha256(args.binary.read_bytes()).hexdigest()
            or old_args != new_args
            or previous_config != config_digest
        ):
            parser.error("--resume requires matching binaries and arguments")
    metadata_path.write_text(
        json.dumps(
            {
                "revision": revision,
                "source_diff_sha256": hashlib.sha256(patch).hexdigest(),
                "gem5_sha256": digest.hexdigest(),
                "guest_argument_bytes": 128,
                "simulator_build": simulator_build,
                "workload_sha256": hashlib.sha256(
                    args.binary.read_bytes()
                ).hexdigest(),
                "configuration_sha256": config_digest,
                "arguments": arguments,
            },
            indent=2,
        )
        + "\n"
    )
    cases = []
    for mode in args.modes:
        for size in args.sizes:
            for offset in args.offsets:
                reps = max(4, min(256, 262144 // max(1, size)))
                for method in args.methods:
                    cases.append((method, size, offset, mode, reps))

    def run(case):
        method, size, offset, mode, reps = case
        directory = args.out / f"{mode}-{size}-{offset}-{method}"
        directory.mkdir(exist_ok=True)
        command = [
            str(args.gem5.resolve()),
            "--listener-mode=off",
            "-d",
            str(directory),
            str(config),
            "--scalar-threshold",
            str(args.scalar_threshold),
            "--vlen",
            str(args.vlen),
            "--prefetch",
            args.prefetch,
            "--store-reserve",
            str(args.store_reserve),
            "--buffers",
            str(args.buffers),
            *(["--no-coalesce"] if args.no_coalesce else []),
            *(["--store-priority"] if args.store_priority else []),
            *(["--no-store-priority"] if args.no_store_priority else []),
            *(["--reference"] if args.reference else []),
            *(["--quad-batch"] if args.quad_batch else []),
            *(["--pair-control"] if args.pair_control else []),
            "--bytes",
            str(args.bytes),
            *(["--pack", str(args.pack)] if args.pack else []),
            "--unroll",
            str(args.unroll),
            str(args.binary.resolve()),
            method,
            str(size),
            str(offset),
            mode,
            str(reps),
        ]
        command_path = directory / "command.json"
        log_path = directory / "run.log"
        cached = (
            args.resume
            and command_path.exists()
            and log_path.exists()
            and "PASS:" in log_path.read_text()
            and "Exit: exiting with last active thread context"
            in log_path.read_text()
            and (directory / "config.json").exists()
            and (directory / "stats.txt").exists()
        )
        if cached:
            try:
                cached = json.loads(command_path.read_text()) == command
                json.loads((directory / "config.json").read_text())
                cached = (
                    cached
                    and "End Simulation Statistics"
                    in (directory / "stats.txt").read_text()
                )
            except (OSError, ValueError):
                cached = False
        command_path.write_text(json.dumps(command, indent=2) + "\n")
        start = time.monotonic()
        returncode = 0
        if not cached:
            with log_path.open("w") as log:
                result = subprocess.run(
                    command, stdout=log, stderr=log, timeout=args.timeout
                )
                returncode = result.returncode
        log = log_path.read_text()
        if returncode or "PASS:" not in log:
            raise RuntimeError(f"{directory}: exit {returncode}")
        guest_commands = []
        memory_interfaces = {}

        def find_commands(obj):
            if isinstance(obj, dict):
                if "cmd" in obj:
                    guest_commands.append(obj["cmd"])
                if obj.get("type") == "DRAMInterface":
                    memory_interfaces[obj["path"]] = {
                        key: obj[key]
                        for key in (
                            "path",
                            "device_bus_width",
                            "devices_per_rank",
                            "ranks_per_channel",
                            "banks_per_rank",
                            "bank_groups_per_rank",
                            "tCK",
                            "tRCD",
                            "tCL",
                            "tRP",
                            "tBURST",
                            "read_buffer_size",
                            "write_buffer_size",
                            "addr_mapping",
                            "range",
                        )
                    }
                for value in obj.values():
                    find_commands(value)
            elif isinstance(obj, list):
                for value in obj:
                    find_commands(value)

        find_commands(json.loads((directory / "config.json").read_text()))
        arguments = [method, str(size), str(offset), mode, str(reps)]
        argument_bytes = sum(len(arg.encode()) + 1 for arg in arguments)
        program_name = "zimemops-bench".ljust(127 - argument_bytes, "_")
        assert len(program_name) + 1 + argument_bytes == 128
        expected_command = [program_name, *arguments]
        assert guest_commands == [expected_command], (
            directory,
            guest_commands,
        )
        memory_parameters = list(memory_interfaces.values())
        expected = (2, 4, 1, 32, 312, 14375)
        assert len(memory_parameters) == expected[0], memory_parameters
        for interface in memory_parameters:
            actual = tuple(
                interface[key]
                for key in (
                    "devices_per_rank",
                    "ranks_per_channel",
                    "banks_per_rank",
                    "tCK",
                    "tRCD",
                )
            )
            assert actual == expected[1:], (directory, actual, expected)
        bus_bits = sum(
            interface["device_bus_width"] * interface["devices_per_rank"]
            for interface in memory_parameters
        )
        assert bus_bits == 64, (directory, bus_bits)
        (directory / "dram-parameters.json").write_text(
            json.dumps(memory_parameters, indent=2) + "\n"
        )
        # The first dump is generated by WORKEND; the automatic final dump
        # also includes post-ROI verification and must not be used.
        stats = {}
        for line in (directory / "stats.txt").read_text().splitlines():
            if "End Simulation Statistics" in line:
                break
            fields = line.split()
            if len(fields) >= 2 and not line.startswith("-"):
                try:
                    stats[fields[0]] = float(fields[1])
                except ValueError:
                    pass
        # Locate the DcachePort stats even if its group name changes.
        peak = max(
            (v for k, v in stats.items() if k.endswith("dataBufferPeakBytes")),
            default=0,
        )
        assert 0 <= peak <= args.buffers * 64, (directory, peak)
        if size:
            assert peak > 0, directory
        read_peak = max(
            (
                v
                for k, v in stats.items()
                if k.endswith("peakLoadBytesPerCycle")
            ),
            default=0,
        )
        write_peak = max(
            (
                v
                for k, v in stats.items()
                if k.endswith("peakStoreBytesPerCycle")
            ),
            default=0,
        )
        assert read_peak <= 32 and write_peak <= 32, (
            directory,
            read_peak,
            write_peak,
        )
        stalls = sum(
            v for k, v in stats.items() if k.endswith("dataBufferStalls")
        )
        # Slot wrapping belongs in address increments, not a per-copy
        # divider. None of these kernels contains an integer divide.
        divide_counts = [
            v
            for k, v in stats.items()
            if k.endswith("commit.committedInstType_0::IntDiv")
        ]
        assert len(divide_counts) == 1, (directory, divide_counts)
        integer_divs = divide_counts[0]
        assert integer_divs == 0, (directory, integer_divs)
        cycles = stats["simTicks"] / 500  # The 2 GHz core's clock period.
        row = dict(
            memory="ddr5-6400",
            memory_channels=len(memory_parameters),
            memory_bus_bits=bus_bits,
            dram_tck_ps=memory_parameters[0]["tCK"],
            dram_trcd_ps=memory_parameters[0]["tRCD"],
            integer_div_ops=integer_divs,
            scalar_threshold=args.scalar_threshold,
            streaming=not args.reference,
            quad_batch=args.quad_batch,
            pair_control=args.pair_control,
            store_priority=args.store_priority and not args.no_store_priority,
            prefetch=args.prefetch,
            buffers=args.buffers,
            store_reserve=args.store_reserve,
            buffer_slots=args.buffers * 2,
            slot_bytes=32,
            coalesce=not args.no_coalesce,
            peak_read_bytes=read_peak,
            peak_write_bytes=write_peak,
            buffer_peak_bytes=peak,
            buffer_stalls=stalls,
            beat_bytes=min(args.vlen // 8, 32),
            vlen=args.vlen,
            transfer_bytes=min(args.bytes, 2 * min(args.vlen // 8, 32)),
            window_bytes=min(args.bytes, 2 * min(args.vlen // 8, 32))
            * args.unroll,
            pack=args.pack,
            unroll=args.unroll,
            method=method,
            operation="mmove" if mode == "overlap" else "mcopy",
            size=size,
            offset=offset,
            mode=mode,
            reps=reps,
            cycles=cycles,
            cycles_per_copy=cycles / reps,
            bytes_per_cycle=size * reps / cycles,
            sim_insts=stats["simInsts"],
            sim_ops=stats["simOps"],
            branch_mispredicts=sum(
                v
                for k, v in stats.items()
                if k.endswith("commit.branchMispredicts")
            ),
            store_reserve_stalls=sum(
                v for k, v in stats.items() if k.endswith("storeReserveStalls")
            ),
            l2_read_ex_requests=stats.get(
                "board.cache_hierarchy.l2-cache-0.ReadExReq.accesses::total",
                0,
            ),
            squashed_loads=sum(
                v for k, v in stats.items() if k.endswith("squashedLoads")
            ),
            host_seconds=None if cached else time.monotonic() - start,
            cached=cached,
        )
        (directory / "roi.json").write_text(json.dumps(row, indent=2) + "\n")
        return row

    rows = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = {pool.submit(run, case): case for case in cases}
        for future in concurrent.futures.as_completed(futures):
            row = future.result()
            rows.append(row)
            print(
                f"{len(rows)}/{len(cases)} {futures[future]} "
                f"{row['cycles_per_copy']:.2f} cycles/copy",
                flush=True,
            )
    rows.sort(key=lambda r: (r["mode"], r["size"], r["offset"], r["method"]))
    (args.out / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
    with (args.out / "results.csv").open("w", newline="") as output:
        writer = csv.DictWriter(
            output, fieldnames=list(rows[0]), lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)


if __name__ == "__main__":
    main()
