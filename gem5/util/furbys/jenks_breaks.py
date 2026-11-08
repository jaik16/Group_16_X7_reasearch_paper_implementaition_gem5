#!/usr/bin/env python3
"""
Jenks Natural Breaks Clustering for FURBYS Hit-Rate Profiling.
HPCA 2025: Efficient Micro-op Cache Replacement Policies for Data Center Applications.

This algorithm partitions continuous hit-rate values into k distinct classes (0 to 7, 3 bits)
by minimizing the sum of squared deviations from the class mean (within-class variance)
and maximizing between-class variance.
"""

import sys
from typing import List, Tuple, Dict


def jenks_natural_breaks(values: List[float], num_classes: int = 8) -> List[float]:
    """
    Computes class break thresholds using Fisher-Jenks dynamic programming.
    Returns sorted list of class boundary thresholds.
    """
    if not values:
        return [0.0] * (num_classes + 1)

    values = sorted(values)
    n = len(values)

    if n <= num_classes:
        # Fewer data points than classes: assign distinct boundaries
        boundaries = [values[0]]
        for val in values:
            if val not in boundaries:
                boundaries.append(val)
        while len(boundaries) <= num_classes:
            boundaries.append(values[-1])
        return sorted(boundaries)

    # Initialize matrices for dynamic programming
    # mat1[i][j]: lower class limit
    # mat2[i][j]: variance
    mat1 = [[0] * (num_classes + 1) for _ in range(n + 1)]
    mat2 = [[float("inf")] * (num_classes + 1) for _ in range(n + 1)]

    for i in range(1, num_classes + 1):
        mat1[1][i] = 1
        mat2[1][i] = 0.0

    v = 0.0
    for l in range(2, n + 1):
        s1 = 0.0
        s2 = 0.0
        w = 0.0
        for m in range(1, l + 1):
            i3 = l - m + 1
            val = values[i3 - 1]
            s2 += val * val
            s1 += val
            w += 1.0
            v = s2 - (s1 * s1) / w
            i4 = i3 - 1
            if i4 != 0:
                for j in range(2, num_classes + 1):
                    if mat2[l][j] >= (v + mat2[i4][j - 1]):
                        mat1[l][j] = i3
                        mat2[l][j] = v + mat2[i4][j - 1]
        mat1[l][1] = 1
        mat2[l][1] = v

    # Extract class breaks
    k = n
    kclass = [0.0] * (num_classes + 1)
    kclass[num_classes] = values[n - 1]
    kclass[0] = values[0]

    count_num = num_classes
    while count_num >= 2:
        id_idx = int(mat1[k][count_num]) - 1
        kclass[count_num - 1] = values[id_idx]
        k = int(mat1[k][count_num] - 1)
        count_num -= 1

    return kclass


def assign_jenks_weights(
    hit_rates: Dict[int, float], num_classes: int = 8
) -> Dict[int, int]:
    """
    Given a mapping of {address: hit_rate}, computes Jenks Natural Breaks
    and assigns an integer weight in [0, num_classes - 1] to each address.
    Weight 0 = coldest (lowest hit rate)
    Weight 7 = hottest (highest hit rate)
    """
    if not hit_rates:
        return {}

    values = list(hit_rates.values())
    breaks = jenks_natural_breaks(values, num_classes)

    weights = {}
    for addr, rate in hit_rates.items():
        weight = 0
        for i in range(1, len(breaks)):
            if rate >= breaks[i - 1]:
                weight = i - 1
        if rate >= breaks[-1]:
            weight = num_classes - 1
        weights[addr] = min(weight, num_classes - 1)

    return weights


def write_profile_file(weights: Dict[int, int], output_filename: str):
    """
    Writes formatted profile file:
    # addr weight
    0x401000 7
    """
    with open(output_filename, "w") as f:
        f.write("# FURBYS Hit-Rate Profile (Jenks Natural Breaks 8 Classes)\n")
        f.write("# Address (hex)    Weight (0-7)\n")
        for addr in sorted(weights.keys()):
            f.write(f"0x{addr:x} {weights[addr]}\n")


if __name__ == "__main__":
    # Self-test example
    test_rates = {
        0x1000: 0.95,
        0x1020: 0.88,
        0x1040: 0.82,
        0x1060: 0.50,
        0x1080: 0.45,
        0x10A0: 0.12,
        0x10C0: 0.05,
        0x10E0: 0.01,
        0x1100: 0.98,
        0x1120: 0.35,
    }
    assigned = assign_jenks_weights(test_rates, 8)
    print("Jenks Weight Assignment:")
    for addr, w in sorted(assigned.items()):
        print(f"  Addr 0x{addr:x}: hit_rate = {test_rates[addr]:.2f} -> Group {w}")

