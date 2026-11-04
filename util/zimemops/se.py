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

"""RISC-V memcpy comparison: identical O3, cache, memory, and VLEN."""

import argparse

import m5
from m5.objects import WriteAllocator
from m5.objects.FuncUnitConfig import (
    RdWrPort,
    ReadPort,
    WritePort,
)
from m5.params import NULL

from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.cachehierarchies.classic.private_l1_private_l2_cache_hierarchy import (
    PrivateL1PrivateL2CacheHierarchy,
)
from gem5.components.memory.single_channel import DIMM_DDR5_6400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import BinaryResource
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cpu", choices=["o3", "timing", "atomic"], default="o3")
parser.add_argument("--disable-zimemops", action="store_true")
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
parser.add_argument(
    "--prefetch", choices=["none", "read", "all"], default="none"
)
parser.add_argument("--unroll", type=int, default=16, choices=range(1, 65))
parser.add_argument("binary")
parser.add_argument("arguments", nargs="*")
args = parser.parse_args()

processor = SimpleProcessor(
    cpu_type={
        "o3": CPUTypes.O3,
        "timing": CPUTypes.TIMING,
        "atomic": CPUTypes.ATOMIC,
    }[args.cpu],
    isa=ISA.RISCV,
    num_cores=1,
    clk_freq="2GHz",
)
for wrapper in processor.get_cores():
    core = wrapper.get_simobject()
    for decoder in core.decoder:
        decoder.zimemops_unroll = args.unroll
        decoder.zimemops_bytes = args.bytes
        decoder.zimemops_streaming = not args.reference
        decoder.zimemops_quad_batch = args.quad_batch
        decoder.zimemops_pair_control = args.pair_control
        decoder.zimemops_scalar_threshold = args.scalar_threshold
    for isa in core.isa:
        isa.vlen = args.vlen
        if not args.disable_zimemops:
            isa.extra_extensions = list(isa.extra_extensions) + ["Zimemops"]
    if args.cpu == "o3":
        for unit in core.instQueues[0].fuPool.FUList:
            if isinstance(unit, RdWrPort):
                unit.count = 0
            elif isinstance(unit, (ReadPort, WritePort)):
                unit.count = 1
        core.cacheLoadPorts = 1
        core.cacheStorePorts = 1
        core.lsqStorePriority = (
            args.store_priority and not args.no_store_priority
        )
        core.lsqDataBufferSlots = args.buffers * 2
        core.lsqDataBufferSlotBytes = 32
        core.lsqStoreReserveSlots = args.store_reserve
        core.recvRespThrottling = True
        core.recvRespMaxCachelines = 2
        core.recvRespBufferSize = 32

        if args.pack:
            core.vecMemPackWidth = args.pack
        core.fetchWidth = 4
        core.decodeWidth = 4
        core.renameWidth = 4
        core.dispatchWidth = 4
        core.issueWidth = 4
        core.wbWidth = 4
        core.commitWidth = 4
        core.forwardComSize = 32
        core.backComSize = 32
        core.numROBEntries = 192
        core.instQueues[0].numEntries = 64
        core.LQEntries = 32
        core.SQEntries = 32


class BenchmarkCaches(PrivateL1PrivateL2CacheHierarchy):
    def incorporate_cache(self, board):
        super().incorporate_cache(board)
        for name in ("l1i-cache-0", "l1d-cache-0", "l2-cache-0"):
            cache = getattr(self, name)
            if args.prefetch == "none":
                cache.prefetcher = NULL
            elif args.prefetch == "read" and hasattr(
                cache.prefetcher, "on_write"
            ):
                cache.prefetcher.on_write = False
        if not args.no_coalesce:
            getattr(self, "l1d-cache-0").write_allocator = WriteAllocator(
                coalesce_limit=0,
                no_allocate_limit=1 << 30,
                delay_threshold=4,
            )


board = SimpleBoard(
    clk_freq="2GHz",
    processor=processor,
    memory=DIMM_DDR5_6400(size="512MiB"),
    cache_hierarchy=BenchmarkCaches(
        l1d_size="32KiB", l1i_size="32KiB", l2_size="256KiB"
    ),
)
board.set_se_binary_workload(
    BinaryResource(local_path=args.binary),
    arguments=args.arguments,
)
# Argument strings participate in the guest stack layout. Pad argv[0] so
# paths, method names and sizes do not move the benchmark's stack frame.
# The board retains the real binary path in Process.executable.
argument_bytes = sum(len(arg.encode()) + 1 for arg in args.arguments)
program_name = "zimemops-bench".ljust(127 - argument_bytes, "_")
for core in processor.get_cores():
    core.get_simobject().workload[0].cmd = [program_name, *args.arguments]


def begin():
    while True:
        m5.stats.reset()
        yield False


def end():
    while True:
        m5.stats.dump()
        yield False


simulator = Simulator(
    board=board,
    on_exit_event={
        ExitEvent.WORKBEGIN: begin(),
        ExitEvent.WORKEND: end(),
    },
)
simulator.run(max_ticks=10**13)
print("Exit:", simulator.get_last_exit_event_cause())
if (
    simulator.get_last_exit_event_cause()
    != "exiting with last active thread context"
):
    raise SystemExit(1)
raise SystemExit(simulator.get_last_exit_event_code())
