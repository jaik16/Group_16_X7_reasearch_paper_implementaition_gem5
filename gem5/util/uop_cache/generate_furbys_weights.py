#!/usr/bin/env python3
# Copyright (c) 2026 Group 16 (Kabir Gupta, Jayesh Vijaivargia, Mahendra)
# All rights reserved.
"""
FURBYS Offline Profile Generator (HPCA 2025)
Pipeline:
  1. Reads PW execution trace from gem5 simulation
  2. Runs Cost-Aware Greedy (FLACK-inspired) replacement policy
  3. Computes hit rate for each Prediction Window
  4. Applies Jenks Natural Breaks per cache set to cluster hit rates into 8 weight buckets (0 to 7)
  5. Outputs formatted weight file for gem5 FURBYS simulation:
       0x<addr> <weight:0..7>
"""

import sys
import os
import collections
from flack_offline_solver import parse_trace_and_solve
from jenks_natural_breaks import assign_weights

def generate_weights(trace_path, output_path, num_sets=64, num_ways=8, num_groups=8, set_shift=2):
    print(f"[FURBYS Profile] Reading trace from {trace_path}...")
    hit_rates = parse_trace_and_solve(trace_path, num_sets=num_sets, num_ways=num_ways)

    print(f"[FURBYS Profile] Replayed {len(hit_rates)} unique Prediction Windows under FLACK heuristic.")
    print(f"[FURBYS Profile] Partitioning into {num_sets} sets and clustering per-set into {num_groups} Jenks Natural Break groups...")

    # Group PW hit rates by cache set
    sets_hit_rates = collections.defaultdict(dict)
    for pc, hr in hit_rates.items():
        s = (pc >> set_shift) % num_sets
        sets_hit_rates[s][pc] = hr

    weights = {}
    for s in range(num_sets):
        if s in sets_hit_rates:
            set_weights = assign_weights(sets_hit_rates[s], num_groups=num_groups)
            weights.update(set_weights)

    print(f"[FURBYS Profile] Writing weight profile to {output_path}...")
    with open(output_path, 'w') as f:
        f.write("# FURBYS Profile Weights generated via FLACK heuristic and per-set Jenks Natural Breaks\n")
        f.write("# Format: 0x<PC> <Weight_0_to_7>\n")
        for pc, w in sorted(weights.items()):
            f.write(f"0x{pc:x} {w}\n")

    print(f"[FURBYS Profile] Successfully generated weights for {len(weights)} PWs!")

    # Summary histogram
    group_counts = [0] * num_groups
    for w in weights.values():
        group_counts[w] += 1
    print("\nWeight Distribution:")
    for g, count in enumerate(group_counts):
        bar = "#" * int(count / max(1, len(weights)) * 40)
        print(f"  Group {g} (weight {g}): {count:5d} PWs {bar}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: generate_furbys_weights.py <input_trace.txt> <output_weights.txt> [num_sets=64] [num_ways=8]")
        sys.exit(1)

    trace_file = sys.argv[1]
    out_file = sys.argv[2]
    num_sets = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    num_ways = int(sys.argv[4]) if len(sys.argv) > 4 else 8

    generate_weights(trace_file, out_file, num_sets=num_sets, num_ways=num_ways)
