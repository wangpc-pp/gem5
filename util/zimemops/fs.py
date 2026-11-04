# Copyright (c) 2026 ByteDance
# Copyright (c) 2026 The Regents of the University of California
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

import argparse
import sys

import m5
from m5.objects import (
    AddrRange,
    Bridge,
    Cache,
    HiFive,
    IOXBar,
    MemCtrl,
    PMAChecker,
    RiscvBareMetal,
    RiscvRTC,
    RiscvSemihosting,
    RiscvSystem,
    Root,
    SrcClockDomain,
    SystemXBar,
    VoltageDomain,
    WriteAllocator,
)
from m5.objects.FuncUnitConfig import (
    RdWrPort,
    ReadPort,
    WritePort,
)
from m5.objects.RiscvCPU import (
    RiscvAtomicSimpleCPU,
    RiscvO3CPU,
    RiscvTimingSimpleCPU,
)
from m5.util.convert import toMemorySize

from gem5.components.memory.dram_interfaces.ddr5 import DDR5_6400_4x8

parser = argparse.ArgumentParser(
    description="Run a RISC-V bare-metal unit-test binary."
)
parser.add_argument("binary", help="The bare-metal RISC-V binary to run.")
parser.add_argument(
    "--mem-size",
    default="128MiB",
    help="Amount of memory to attach to the test system.",
)

parser.add_argument("--cpu", choices=["o3", "timing", "atomic"], default="o3")
parser.add_argument("--rv32", action="store_true")
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
parser.add_argument("--publish-period", type=int, default=0)
parser.add_argument(
    "--prefetch", choices=["none", "read", "all"], default="none"
)
parser.add_argument("--unroll", type=int, default=16, choices=range(1, 65))
args = parser.parse_args()
if args.publish_period < 0:
    parser.error("--publish-period must be nonnegative")

memory_start = 0x7FFFF000
reset_vector_size = 0x1000

system = RiscvSystem()
system.mem_mode = "atomic" if args.cpu == "atomic" else "timing"
system.mem_ranges = [
    AddrRange(
        start=memory_start,
        size=toMemorySize(args.mem_size) + reset_vector_size,
    )
]

system.workload = RiscvBareMetal(
    bootloader=args.binary,
    semihosting=RiscvSemihosting(cmd_line=args.binary),
)

system.iobus = IOXBar()
system.membus = SystemXBar()
system.system_port = system.membus.cpu_side_ports

system.platform = HiFive()
system.platform.rtc = RiscvRTC(frequency="100MHz")
system.platform.clint.int_pin = system.platform.rtc.int_pin
system.platform.pci_host.internal_connect()
system.platform.pci_host.connect_upper_bus(system.iobus, True)
system.platform.attachOnChipIO(system.membus)
system.platform.attachOffChipIO(system.iobus)
system.platform.attachPlic()
system.platform.setNumCores(1)

system.bridge = Bridge(delay="50ns")
system.bridge.mem_side_port = system.iobus.cpu_side_ports
system.bridge.cpu_side_port = system.membus.mem_side_ports
system.bridge.ranges = system.platform._off_chip_ranges()

system.iobridge = Bridge(delay="50ns", ranges=system.mem_ranges)
system.iobridge.cpu_side_port = system.iobus.mem_side_ports
system.iobridge.mem_side_port = system.membus.cpu_side_ports

system.cache_line_size = 64
system.voltage_domain = VoltageDomain(voltage="1V")
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=system.voltage_domain
)
system.cpu_voltage_domain = VoltageDomain()
system.cpu_clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=system.cpu_voltage_domain
)

cpu_class = {
    "o3": RiscvO3CPU,
    "timing": RiscvTimingSimpleCPU,
    "atomic": RiscvAtomicSimpleCPU,
}[args.cpu]
system.cpu = cpu_class(clk_domain=system.cpu_clk_domain, cpu_id=0)
system.cpu.createThreads()
for decoder in system.cpu.decoder:
    decoder.zimemops_unroll = args.unroll
    decoder.zimemops_bytes = args.bytes
    decoder.zimemops_streaming = not args.reference
    decoder.zimemops_quad_batch = args.quad_batch
    decoder.zimemops_pair_control = args.pair_control
    decoder.zimemops_scalar_threshold = args.scalar_threshold
if args.cpu == "o3":
    for unit in system.cpu.instQueues[0].fuPool.FUList:
        if isinstance(unit, RdWrPort):
            unit.count = 0
        elif isinstance(unit, (ReadPort, WritePort)):
            unit.count = 1
    system.cpu.cacheLoadPorts = 1
    system.cpu.cacheStorePorts = 1
    system.cpu.lsqStorePriority = (
        args.store_priority and not args.no_store_priority
    )
    system.cpu.lsqDataBufferSlots = args.buffers * 2
    system.cpu.lsqDataBufferSlotBytes = 32
    system.cpu.lsqStoreReserveSlots = args.store_reserve
    system.cpu.recvRespThrottling = True
    system.cpu.recvRespMaxCachelines = 2
    system.cpu.recvRespBufferSize = 32
if args.cpu == "o3" and args.pack:
    system.cpu.vecMemPackWidth = args.pack
for isa in system.cpu.isa:
    isa.vlen = args.vlen
    if args.rv32:
        isa.riscv_profile = "RVI20U32"
        isa.extra_extensions = ["M", "A", "F", "D", "C", "Zicsr", "Zifencei"]
    if not args.disable_zimemops:
        isa.extra_extensions = list(isa.extra_extensions) + ["Zimemops"]
for kind in ("icache", "dcache"):
    cache = Cache(
        size="32KiB",
        assoc=8,
        tag_latency=2,
        data_latency=2,
        response_latency=2,
        mshrs=16,
        tgts_per_mshr=16,
    )
    if kind == "dcache" and not args.no_coalesce:
        cache.write_allocator = WriteAllocator(
            coalesce_limit=0, no_allocate_limit=1 << 30, delay_threshold=4
        )
    setattr(system, kind, cache)
    setattr(system.cpu, kind + "_port", cache.cpu_side)
    cache.mem_side = system.membus.cpu_side_ports
system.cpu.createInterruptController()
system.cpu.mmu.itb.walker.port = system.membus.cpu_side_ports
system.cpu.mmu.dtb.walker.port = system.membus.cpu_side_ports

uncacheable_range = [
    *system.platform._on_chip_ranges(),
    *system.platform._off_chip_ranges(),
]
system.cpu.mmu.pma_checker = PMAChecker(uncacheable=uncacheable_range)

system.mem_ctrl = [MemCtrl(), MemCtrl()]
for channel, controller in enumerate(system.mem_ctrl):
    controller.dram = DDR5_6400_4x8(
        range=AddrRange(
            system.mem_ranges[0].start,
            size=system.mem_ranges[0].size(),
            intlvHighBit=6,
            intlvBits=1,
            intlvMatch=channel,
        )
    )
    controller.port = system.membus.mem_side_ports

root = Root(full_system=True, system=system)

m5.instantiate()

publications = 0
while True:
    exit_event = m5.simulate(args.publish_period or 10**11)
    if (
        args.publish_period
        and exit_event.getCause() == "simulate() limit reached"
    ):
        system.cpu.synchronizeState(0)
        publications += 1
        continue
    break
if args.publish_period:
    print(f"Progress publications: {publications}")
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}.")
sys.exit(
    exit_event.getCode()
    if exit_event.getCause() == "m5_fail instruction encountered"
    else 99
)
