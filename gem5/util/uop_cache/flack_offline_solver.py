#!/usr/bin/env python3
# Copyright (c) 2026 Group 16 (Kabir Gupta, Jayesh Vijaivargia, Mahendra)
# All rights reserved.
"""
Cost-Aware Greedy (FLACK-inspired) Policy Simulator
Based on HPCA 2025:
"From Optimal to Practical: Efficient Micro-op Cache Replacement Policies
 for Data Center Applications" by Kan Zhu et al.

This simulator models a Cost-Aware Greedy (FLACK-inspired) policy with three
micro-op cache specific features:
  1. Variable disproportional costs: unit cost = cost / size (uops / entries)
  2. Selective bypass: prioritize keeping larger windows, bypass if unit cost is lower than cache
  3. Lazy eviction: delay eviction until insertion time

Note: This is an online/trace-based greedy heuristic derived from FLACK's principles,
not the full offline Flow-based Optimal (FOO) min-cost circulation solver.
"""

import sys
import collections

class PredictionWindow:
    def __init__(self, start_pc, cost, size):
        self.start_pc = start_pc
        self.cost = max(1, cost)
        self.size = max(1, size)
        self.unit_cost = self.cost / self.size  # uops per entry

class CostAwareGreedySimulator:
    def __init__(self, num_sets=64, num_ways=8, uops_per_entry=8):
        self.num_sets = num_sets
        self.num_ways = num_ways
        self.uops_per_entry = uops_per_entry
        self.set_mask = num_sets - 1
        self.set_shift = 2

        # Cache state: per-set list of cached windows
        self.cache = {s: [] for s in range(num_sets)}

        # Statistics per PC: {pc: {'accesses': N, 'hits': H, 'uop_hits': UH, 'uop_misses': UM}}
        self.stats = collections.defaultdict(lambda: {
            'accesses': 0, 'hits': 0, 'uop_hits': 0, 'uop_misses': 0
        })

    def get_set(self, pc):
        return (pc >> self.set_shift) & self.set_mask

    def access(self, pw):
        pc = pw.start_pc
        s = self.get_set(pc)
        stat = self.stats[pc]
        stat['accesses'] += 1

        cached_set = self.cache[s]

        # Check for hit or partial hit
        hit_idx = -1
        for i, item in enumerate(cached_set):
            if item.start_pc == pc:
                hit_idx = i
                break

        if hit_idx != -1:
            # Hit!
            stat['hits'] += 1
            stat['uop_hits'] += pw.cost
            # If incoming window is larger, update cached window (partial hit optimization)
            if pw.cost > cached_set[hit_idx].cost:
                cached_set[hit_idx] = pw
            return True
        else:
            # Miss!
            stat['uop_misses'] += pw.cost

            # Insertion decision with cost-aware greedy:
            if len(cached_set) < self.num_ways:
                cached_set.append(pw)
            else:
                # All ways full: select victim based on variable unit cost
                min_idx = 0
                min_unit_cost = cached_set[0].unit_cost
                for i in range(1, len(cached_set)):
                    if cached_set[i].unit_cost < min_unit_cost:
                        min_unit_cost = cached_set[i].unit_cost
                        min_idx = i

                # Selective bypass:
                # If incoming window's unit cost is lower than everything in cache, bypass!
                if pw.unit_cost < min_unit_cost:
                    # Bypass insertion
                    pass
                else:
                    # Evict victim and insert pw (lazy eviction at insertion time)
                    cached_set[min_idx] = pw
            return False

    def compute_hit_rates(self):
        """Returns dict: pc -> hit_rate (0.0 to 1.0)"""
        hit_rates = {}
        for pc, s in self.stats.items():
            if s['accesses'] > 0:
                hit_rates[pc] = s['hits'] / s['accesses']
            else:
                hit_rates[pc] = 0.0
        return hit_rates

def parse_trace_and_solve(trace_path, num_sets=64, num_ways=8):
    """
    Two-pass trace processor:
    Pass 1: Collect (cost, size) metadata for each PC from INSERT lines.
    Pass 2: Replay only LOOKUP lines to simulate the exact access sequence.
    """
    sim = CostAwareGreedySimulator(num_sets=num_sets, num_ways=num_ways)
    pw_info = {}

    # Pass 1: Collect PW metadata
    with open(trace_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            if len(parts) >= 4 and parts[0] in ("INSERT", "PW"):
                pc = int(parts[1], 16) if parts[1].startswith("0x") else int(parts[1])
                cost = int(parts[2])
                size = int(parts[3])
                # Keep the largest cost observed for this PC
                if pc not in pw_info or cost > pw_info[pc][0]:
                    pw_info[pc] = (cost, size)

    # Pass 2: Replay LOOKUP accesses
    with open(trace_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            if len(parts) >= 2 and parts[0] == "LOOKUP":
                pc = int(parts[1], 16) if parts[1].startswith("0x") else int(parts[1])
                cost, size = pw_info.get(pc, (1, 1))
                sim.access(PredictionWindow(pc, cost, size))

    return sim.compute_hit_rates()

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: flack_offline_solver.py <trace_file> [num_sets=64] [num_ways=8]")
        sys.exit(1)

    trace_file = sys.argv[1]
    num_sets = int(sys.argv[2]) if len(sys.argv) > 2 else 64
    num_ways = int(sys.argv[3]) if len(sys.argv) > 3 else 8

    hit_rates = parse_trace_and_solve(trace_file, num_sets, num_ways)
    print(f"Processed {len(hit_rates)} unique Prediction Windows under Cost-Aware Greedy (FLACK-inspired).")
    for pc, hr in sorted(hit_rates.items(), key=lambda x: x[1], reverse=True)[:20]:
        print(f"  PC: 0x{pc:x} -> Hit Rate: {hr*100:.2f}%")
