#!/usr/bin/env python3
"""
FLACK: FOO-based seLectively-bypassing Asynchronizing Cost-varying selective-data-Keeping.
HPCA 2025: Offline Near-Optimal Micro-op Cache Replacement Policy.

Calculates near-optimal replacement decisions considering:
1. Disproportionate / variable uop cost (cost / size).
2. Partial hits from overlapping prediction windows.
3. Lazy evictions to mitigate asynchronous lookup/insertion latency.
"""

from typing import List, Dict, Tuple, Optional
from collections import defaultdict
import math


class PredictionWindow:
    def __init__(self, addr: int, cost: int, size: int):
        self.addr = addr
        self.cost = cost      # Number of micro-ops (e.g. 1-32)
        self.size = size      # Number of cache entries (e.g. 1-4)
        self.cost_per_entry = cost / max(1, size)

    def __repr__(self):
        return f"PW(0x{self.addr:x}, cost={self.cost}, size={self.size})"


class FLACKSimulator:
    def __init__(self, num_sets: int = 64, num_ways: int = 8, uop_per_entry: int = 8):
        self.num_sets = num_sets
        self.num_ways = num_ways
        self.uop_per_entry = uop_per_entry

    def get_set(self, addr: int) -> int:
        # Micro-op cache set indexing (typically based on instruction address)
        return (addr >> 6) % self.num_sets

    def run_flack(self, access_trace: List[PredictionWindow]) -> Dict[str, any]:
        """
        Simulates FLACK offline near-optimal replacement over a trace.
        """
        # 1. Precompute next access distances for Belady/FOO-like forward analysis
        future_accesses = defaultdict(list)
        for idx, pw in enumerate(access_trace):
            future_accesses[pw.addr].append(idx)

        # Cache state: per-set list of resident PredictionWindows
        # Each entry stores: (PW, next_access_idx, entry_time)
        cache_sets = [[] for _ in range(self.num_sets)]

        hits = 0
        misses = 0
        partial_hits = 0
        total_uops_requested = 0
        missed_uops = 0

        pw_access_count = defaultdict(int)
        pw_hit_count = defaultdict(int)

        current_future_ptr = defaultdict(int)

        for curr_idx, pw in enumerate(access_trace):
            pw_access_count[pw.addr] += 1
            total_uops_requested += pw.cost
            s_idx = self.get_set(pw.addr)
            target_set = cache_sets[s_idx]

            # Advance future access pointer
            current_future_ptr[pw.addr] += 1
            next_idx = (
                future_accesses[pw.addr][current_future_ptr[pw.addr]]
                if current_future_ptr[pw.addr] < len(future_accesses[pw.addr])
                else float("inf")
            )

            # Check for exact hit or partial hit
            hit_found = False
            for entry_idx, (cached_pw, _, _) in enumerate(target_set):
                if cached_pw.addr == pw.addr:
                    if cached_pw.cost >= pw.cost:
                        # Full or partial hit!
                        hit_found = True
                        if cached_pw.cost > pw.cost:
                            partial_hits += 1
                        hits += 1
                        pw_hit_count[pw.addr] += 1
                        # Update future access
                        target_set[entry_idx] = (cached_pw, next_idx, curr_idx)
                        break
                    else:
                        # Partial window served; needs extension to larger window
                        hit_found = True
                        hits += 1
                        pw_hit_count[pw.addr] += 1
                        # Replace smaller window with larger window (Section IV)
                        target_set[entry_idx] = (pw, next_idx, curr_idx)
                        break

            if hit_found:
                continue

            # Cache Miss!
            misses += 1
            missed_uops += pw.cost

            # Check if set has available capacity
            # In uop cache, each way holds 1 entry. Multi-entry PWs use pw.size ways.
            occupied_entries = sum(item[0].size for item in target_set)
            required_entries = pw.size

            if occupied_entries + required_entries <= self.num_ways:
                # Direct insertion
                target_set.append((pw, next_idx, curr_idx))
            else:
                # Eviction needed: FLACK calculates eviction metric
                # Unit cost = cost / size. Priority = reuse_distance / unit_cost
                # Lines with largest (reuse_distance / (cost/size)) are evicted first.
                while sum(item[0].size for item in target_set) + required_entries > self.num_ways:
                    if not target_set:
                        break
                    # Find candidate with worst utility
                    victim_idx = 0
                    max_score = -1.0
                    for i, (c_pw, c_next, _) in enumerate(target_set):
                        reuse_dist = c_next - curr_idx if c_next != float("inf") else 1e9
                        cost_density = c_pw.cost_per_entry
                        # FLACK metric: high reuse dist and low cost per entry -> best victim
                        score = reuse_dist / max(0.1, cost_density)
                        if score > max_score:
                            max_score = score
                            victim_idx = i

                    # Selective bypass check: if incoming PW score is worse than victim, bypass incoming!
                    incoming_reuse = next_idx - curr_idx if next_idx != float("inf") else 1e9
                    incoming_score = incoming_reuse / max(0.1, pw.cost_per_entry)

                    if incoming_score > max_score:
                        # Bypass insertion to prevent pollution and save insertion energy
                        break
                    else:
                        # Lazy eviction: evict selected victim
                        target_set.pop(victim_idx)

                if sum(item[0].size for item in target_set) + required_entries <= self.num_ways:
                    target_set.append((pw, next_idx, curr_idx))

        # Calculate hit rates per address
        hit_rates = {}
        for addr, count in pw_access_count.items():
            hit_rates[addr] = pw_hit_count[addr] / count if count > 0 else 0.0

        return {
            "total_accesses": len(access_trace),
            "hits": hits,
            "misses": misses,
            "miss_rate": (misses / len(access_trace)) if access_trace else 0.0,
            "partial_hits": partial_hits,
            "total_uops": total_uops_requested,
            "missed_uops": missed_uops,
            "uop_miss_rate": (missed_uops / total_uops_requested) if total_uops_requested else 0.0,
            "hit_rates": hit_rates,
        }


if __name__ == "__main__":
    trace = [
        PredictionWindow(0x1000, 4, 1),
        PredictionWindow(0x2000, 2, 1),
        PredictionWindow(0x1000, 4, 1),
        PredictionWindow(0x3000, 8, 2),
        PredictionWindow(0x1000, 2, 1), # Partial window of 0x1000!
        PredictionWindow(0x4000, 3, 1),
        PredictionWindow(0x1000, 4, 1),
    ]
    sim = FLACKSimulator(num_sets=4, num_ways=4)
    res = sim.run_flack(trace)
    print("FLACK Simulation Result:")
    print(f"  Accesses: {res['total_accesses']}")
    print(f"  Hits: {res['hits']}, Misses: {res['misses']}")
    print(f"  Partial Hits: {res['partial_hits']}")
    print(f"  Micro-op Miss Rate: {res['uop_miss_rate'] * 100:.2f}%")

