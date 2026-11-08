#!/usr/bin/env python3
# Copyright (c) 2026 Group 16 (Kabir Gupta, Jayesh Vijaivargia, Mahendra)
# All rights reserved.
"""
Jenks Natural Breaks (Fisher-Jenks Algorithm) for 1D Data Classification.
Used by FURBYS (HPCA 2025) to partition micro-op cache hit rates into 8 weight buckets (0 to 7).
"""

def jenks_breaks(data, num_classes=8):
    """
    Computes k class break points for a sorted 1D array using the Fisher-Jenks algorithm.
    """
    data = sorted(data)
    n = len(data)

    if n <= num_classes:
        # Trivial case: each unique value or item gets its own class
        breaks = [data[i] for i in range(n)]
        while len(breaks) < num_classes + 1:
            breaks.append(data[-1] if breaks else 0.0)
        return breaks

    # Initialize matrices
    # mat1: variance matrix, mat2: index matrix
    mat1 = [[0.0] * (num_classes + 1) for _ in range(n + 1)]
    mat2 = [[0] * (num_classes + 1) for _ in range(n + 1)]

    for y in range(1, num_classes + 1):
        mat1[1][y] = 0.0
        mat2[1][y] = 1
        for i in range(2, n + 1):
            mat1[i][y] = float('inf')

    v = 0.0
    for l in range(2, n + 1):
        s1 = 0.0
        s2 = 0.0
        w = 0.0
        for m in range(1, l + 1):
            i3 = l - m + 1
            val = float(data[i3 - 1])
            s2 += val * val
            s1 += val
            w += 1.0
            v = s2 - (s1 * s1) / w
            i4 = i3 - 1
            if i4 != 0:
                for j in range(2, num_classes + 1):
                    if mat1[l][j] >= (v + mat1[i4][j - 1]):
                        mat2[l][j] = i3
                        mat1[l][j] = v + mat1[i4][j - 1]
        mat1[l][1] = v
        mat2[l][1] = 1

    k = n
    kclass = [0.0] * (num_classes + 1)
    kclass[num_classes] = data[n - 1]

    countNum = num_classes
    while countNum >= 2:
        id_ = int(mat2[k][countNum]) - 2
        kclass[countNum - 1] = data[id_]
        k = int(mat2[k][countNum]) - 1
        countNum -= 1

    return kclass

def assign_weights(hit_rates, num_groups=8):
    """
    Given a dict: {pc: hit_rate}, computes Jenks Natural Breaks and returns
    dict: {pc: weight_0_to_7}.
    """
    if not hit_rates:
        return {}

    values = sorted(hit_rates.values())
    unique_vals = sorted(list(set(values)))

    if len(unique_vals) <= num_groups:
        val_to_weight = {v: min(num_groups - 1, i) for i, v in enumerate(unique_vals)}
        return {pc: val_to_weight[hr] for pc, hr in hit_rates.items()}

    breaks = jenks_breaks(values, num_classes=num_groups)

    def get_group(val):
        for g in range(num_groups):
            if val <= breaks[g + 1]:
                return g
        return num_groups - 1

    return {pc: get_group(hr) for pc, hr in hit_rates.items()}
