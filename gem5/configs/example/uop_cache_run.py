# Copyright (c) 2026 Group 16 (Kabir Gupta, Jayesh Vijaivargia, Mahendra)
# All rights reserved.
#
# gem5 Simulation Script for Evaluating Micro-op Cache Replacement Policies
# Based on HPCA 2025 paper:
# "From Optimal to Practical: Efficient Micro-op Cache Replacement Policies
#  for Data Center Applications" by Kan Zhu et al.
#
# Configures an AMD Zen3-like x86 O3CPU frontend as specified in Table I:
#  - Micro-op Cache: 512 entries, 8-way (64 sets), 8 uops/entry
#  - 1-cycle switch delay
#  - Strict inclusiveness with L1 instruction cache
#  - Replacement policies: LRU, SRRIP, BELADY, FLACK, FURBYS

import argparse
import sys
import os

import m5
from m5.objects import *

parser = argparse.ArgumentParser(description="Evaluate Micro-op Cache in gem5 O3CPU")

# Simulation Binary
parser.add_argument("--cmd", type=str,
                    default="tests/test-progs/hello/bin/x86/linux/hello",
                    help="Path to executable binary (default: built-in hello)")
parser.add_argument("--options", type=str, default="",
                    help="Command-line arguments for the binary")

# Micro-op Cache Configuration (Table I from paper)
parser.add_argument("--enable-uop-cache", action="store_true", default=True,
                    help="Enable the micro-op cache")
parser.add_argument("--disable-uop-cache", dest="enable_uop_cache", action="store_false",
                    help="Disable micro-op cache (baseline without uop cache)")
parser.add_argument("--uop-entries", type=int, default=512,
                    help="Total entries in micro-op cache (default: 512)")
parser.add_argument("--uop-assoc", type=int, default=8,
                    help="Associativity of micro-op cache (default: 8)")
parser.add_argument("--uop-uops-per-entry", type=int, default=8,
                    help="Micro-ops per entry (default: 8)")
parser.add_argument("--repl-policy", type=str, default="lru",
                    choices=["lru", "srrip", "belady", "flack", "furbys"],
                    help="Replacement policy: lru, srrip, belady, flack, furbys")
parser.add_argument("--uop-switch-delay", type=int, default=1,
                    help="Switch penalty delay cycles on uop cache miss (default: 1)")
parser.add_argument("--uop-decode-delay", type=int, default=5,
                    help="Decoder pipeline latency cycles before PW insertion (default: 5)")
parser.add_argument("--furbys-weight-file", type=str, default="",
                    help="Path to offline-profiled weights file for FURBYS")
parser.add_argument("--uop-trace-file", type=str, default="",
                    help="Path to future access trace file for Belady or FLACK replay")
parser.add_argument("--furbys-k", type=int, default=1,
                    help="Selective bypass threshold K for FURBYS (default: 1)")
parser.add_argument("--pitfall-depth", type=int, default=2,
                    help="Depth of local miss-pitfall detector (default: 2)")
parser.add_argument("--dump-trace", type=str, default="",
                    help="File path to dump PW access trace for offline profiling")

args = parser.parse_args()

# Build System
system = System()
system.clk_domain = SrcClockDomain()
system.clk_domain.clock = "3.2GHz"  # Table I: 3.2GHz
system.clk_domain.voltage_domain = VoltageDomain()

system.mem_mode = "timing"
system.mem_ranges = [AddrRange("8GiB")]  # Table I: 8-GiB DRAM

# Configure CPU with Zen3-like parameters
cpu = X86O3CPU()
cpu.fetchWidth = 8
cpu.decodeWidth = 4      # Table I: 4-wide decoder
cpu.renameWidth = 6      # Table I: 6-wide OoO
cpu.dispatchWidth = 6
cpu.issueWidth = 6
cpu.numROBEntries = 256  # Table I: 256-entry ROB
cpu.instQueues = [IQUnit(numEntries=96)]  # Table I: 96-entry RS
cpu.decoupledFrontEnd = False

# Micro-op Cache Parameters
cpu.enableUopCache = args.enable_uop_cache
cpu.uopCacheEntries = args.uop_entries
cpu.uopCacheAssoc = args.uop_assoc
cpu.uopCacheUopsPerEntry = args.uop_uops_per_entry
cpu.uopCacheReplPolicy = args.repl_policy
cpu.uopCacheSwitchDelay = args.uop_switch_delay
cpu.uopCacheDecodeDelay = args.uop_decode_delay
cpu.uopCacheInclusive = True
cpu.uopCacheFurbysWeightFile = args.furbys_weight_file
cpu.uopCacheTraceFile = args.uop_trace_file
cpu.uopCacheFurbysBypassThreshold = args.furbys_k
cpu.uopCachePitfallDepth = args.pitfall_depth
cpu.uopCacheProfileOutputFile = args.dump_trace

system.cpu = [cpu]

# Caches (Table I: L1i 32KB 8-way 1-cycle, L1d 32KB 8-way 2-cycle, L2 512KB 8-way 16-cycle)
class L1ICache(Cache):
    size = "32KiB"
    assoc = 8
    tag_latency = 1
    data_latency = 1
    response_latency = 1
    mshrs = 4
    tgts_per_mshr = 20
    is_read_only = True
    writeback_clean = True

class L1DCache(Cache):
    size = "32KiB"
    assoc = 8
    tag_latency = 2
    data_latency = 2
    response_latency = 2
    mshrs = 4
    tgts_per_mshr = 20

class L2Cache(Cache):
    size = "512KiB"
    assoc = 8
    tag_latency = 16
    data_latency = 16
    response_latency = 16
    mshrs = 20
    tgts_per_mshr = 12

system.cpu[0].icache = L1ICache()
system.cpu[0].dcache = L1DCache()
system.l2bus = L2XBar()
system.l2cache = L2Cache()

system.cpu[0].icache.cpu_side = system.cpu[0].icache_port
system.cpu[0].dcache.cpu_side = system.cpu[0].dcache_port
system.cpu[0].icache.mem_side = system.l2bus.cpu_side_ports
system.cpu[0].dcache.mem_side = system.l2bus.cpu_side_ports
system.l2cache.cpu_side = system.l2bus.mem_side_ports

system.membus = SystemXBar()
system.l2cache.mem_side = system.membus.cpu_side_ports

# Memory Controller
system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR4_2400_16x4()
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports

system.system_port = system.membus.cpu_side_ports

# Interrupt Controller for x86
system.cpu[0].createInterruptController()
system.cpu[0].interrupts[0].pio = system.membus.mem_side_ports
system.cpu[0].interrupts[0].int_requestor = system.membus.cpu_side_ports
system.cpu[0].interrupts[0].int_responder = system.membus.mem_side_ports

# Set up process
process = Process()
process.executable = args.cmd
process.cmd = [args.cmd] + args.options.split()
system.cpu[0].workload = process
system.cpu[0].createThreads()

system.workload = SEWorkload.init_compatible(args.cmd)

root = Root(full_system=False, system=system)
m5.instantiate()

print("=" * 60)
print("gem5 Micro-op Cache Simulation Starting")
print(f"  Binary:      {args.cmd}")
print(f"  Uop Cache:   {'Enabled' if args.enable_uop_cache else 'Disabled'}")
if args.enable_uop_cache:
    print(f"  Entries:     {args.uop_entries} ({args.uop_assoc}-way)")
    print(f"  Policy:      {args.repl_policy.upper()}")
    if args.repl_policy == 'furbys':
        print(f"  FURBYS File: {args.furbys_weight_file or 'None (using defaults)'}")
        print(f"  Bypass K:    {args.furbys_k}")
        print(f"  Pitfall D:   {args.pitfall_depth}")
print("=" * 60)

exit_event = m5.simulate()
print(f"Simulation finished at tick {m5.curTick()} because: {exit_event.getCause()}")
