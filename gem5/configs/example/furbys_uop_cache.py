# Copyright (c) 2025 Antigravity Research / Authors of HPCA 2025 FURBYS
# All rights reserved.
#
# Configuration script for gem5 demonstrating how to configure and use
# FURBYS and FLACK replacement policies on an L1 instruction / micro-op cache.
#
# Usage:
#   gem5.opt configs/example/furbys_uop_cache.py --policy=FURBYS --profile=profile.txt

import argparse
import sys

import m5
from m5.objects import *
from m5.objects.ReplacementPolicies import (
    LRURP,
    BRRIPRP,
    FURBYSRP,
    FLACKRP,
)


def create_uop_cache(policy_name="FURBYS", profile_path=""):
    """
    Creates a micro-op cache SimObject configuration based on the parameters
    from HPCA 2025 (512 entries, 8-way associativity, 64 sets, 8 uops/entry).
    """
    if policy_name == "FURBYS":
        rp = FURBYSRP(
            num_weight_bits=3,       # 3-bit weights (0-7)
            num_rrpv_bits=2,         # 2-bit SRRIP metadata
            bypass_k=1,              # Bypass threshold margin K = 1
            pitfall_depth=2,         # 2-slot local miss-pitfall detector
            num_sets=64,             # 64 sets (512 total entries / 8 ways)
            profile_file=profile_path,
        )
    elif policy_name == "FLACK":
        rp = FLACKRP(uops_per_entry=8)
    elif policy_name == "SRRIP":
        rp = BRRIPRP(num_bits=2, btp=100)
    else:
        rp = LRURP()

    # Model uop cache using gem5 Cache / BaseSetAssoc tags
    uop_cache = Cache(
        size="32KiB",
        assoc=8,
        tag_latency=1,
        data_latency=1,
        response_latency=1,
        mshrs=16,
        tgts_per_mshr=8,
        replacement_policy=rp,
    )
    return uop_cache


def build_system(policy_name="FURBYS", profile_path=""):
    system = System()
    system.clk_domain = SrcClockDomain()
    system.clk_domain.clock = "3.2GHz"
    system.clk_domain.voltage_domain = VoltageDomain()

    system.mem_mode = "timing"
    system.mem_ranges = [AddrRange("8GiB")]

    # Out-of-order CPU model (resembling AMD Zen3/Zen4)
    system.cpu = X86O3CPU()

    # Assign L1 instruction cache with FURBYS replacement policy
    system.cpu.icache = create_uop_cache(policy_name, profile_path)
    system.cpu.dcache = Cache(size="32KiB", assoc=8)

    # L2 unified cache
    system.l2cache = Cache(size="512KiB", assoc=8)

    # Bus crossbars
    system.membus = SystemXBar()
    system.l2bus = L2XBar()

    # Connect ports
    system.cpu.icache.cpu_side = system.cpu.icache_port
    system.cpu.dcache.cpu_side = system.cpu.dcache_port

    system.cpu.icache.mem_side = system.l2bus.cpu_side_ports
    system.cpu.dcache.mem_side = system.l2bus.cpu_side_ports

    system.l2cache.cpu_side = system.l2bus.mem_side_ports
    system.l2cache.mem_side = system.membus.cpu_side_ports

    system.system_port = system.membus.cpu_side_ports

    # DRAM Memory controller
    system.mem_ctrl = MemCtrl()
    system.mem_ctrl.dram = DDR4_2400_8x8()
    system.mem_ctrl.dram.range = system.mem_ranges[0]
    system.mem_ctrl.port = system.membus.mem_side_ports

    return system


if __name__ == "__m5_main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--policy",
        choices=["LRU", "SRRIP", "FURBYS", "FLACK"],
        default="FURBYS",
        help="Micro-op cache replacement policy",
    )
    parser.add_argument(
        "--profile",
        default="",
        help="Path to offline Jenks Natural Breaks profile file",
    )
    args = parser.parse_args()

    system = build_system(args.policy, args.profile)
    root = Root(full_system=False, system=system)
    m5.instantiate()
    print(f"gem5 initialized with {args.policy} micro-op cache replacement policy.")
    print("Simulation ready.")

