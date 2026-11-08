# FURBYS & FLACK: Efficient Micro-op Cache Replacement Policies in gem5

Implementation of the research paper:
> **"From Optimal to Practical: Efficient Micro-op Cache Replacement Policies for Data Center Applications"**
> *Kan Zhu, Yilong Zhao, Yufei Gao, Peter Braun, Tanvir Ahmed Khan, Heiner Litz, Baris Kasikci, Shuwen Deng*  
> **IEEE International Symposium on High Performance Computer Architecture (HPCA 2025)**

---

## 1. Overview of Key Concepts

### Why Micro-op Caches Matter
Modern x86 processors expend significant power decoding variable-length CISC instructions into RISC-like micro-operations (micro-ops / uops). Modern processors (e.g. AMD Zen3/Zen4, Intel Skylake/Golden Cove) feature a **Micro-op Cache** that stores decoded uops. Hits in the micro-op cache allow clock-gating the power-hungry decoders and L1 instruction cache, improving performance-per-watt (PPW) by up to 7.41%.

### Why Conventional Policies (LRU, Belady, SRRIP) Fail
In large-footprint data center applications (Kafka, Postgres, Clang, Tomcat, etc.), over 99% of micro-op cache misses are capacity/conflict misses. Conventional policies fail because they ignore three critical microarchitectural characteristics:
1. **Variable and Disproportionate Miss Costs & Sizes:** Prediction Windows (PWs) are the fetch unit, but the cache stores fixed-size entries (e.g., 8 uops/entry). A PW spans multiple entries ($size$), and holds a variable number of uops ($cost$). Evicting entries with many uops is much costlier than evicting sparsely populated entries.
2. **Partial Hits & Overlapping Prediction Windows:** Non-taken conditional branches do not terminate PWs, leading to overlapping windows starting at the same address. A larger cached PW can serve hits for smaller requested windows (partial hit).
3. **Asynchronous Lookup & Insertion:** Decode latency causes variable delays between miss detection and insertion.

### The Solutions: FLACK and FURBYS
- **FLACK (Near-Optimal Offline Policy):**
  - Uses a variable-cost metric: $\text{cost} / \text{size}$ (uops per entry).
  - Handles partial hits by keeping larger windows and throttling bypassing.
  - Mitigates asynchrony via lazy evictions.
- **FURBYS (Practical Online Policy):**
  - **Offline Profile-Guided Weights:** Hit rates from execution traces / FLACK decisions are clustered into 8 buckets (weights 0 to 7, 3 bits) using the **Jenks Natural Breaks** algorithm at set granularity.
  - **Local Miss-Pitfall Detector:** A 2-slot buffer per set detects repeated evictions of the same way (transient phase shifts where globally hot uops become locally cold). Upon detection, FURBYS temporarily degrades to **SRRIP** (Static RRIP) for one replacement before switching back.
  - **Selective Bypassing:** Incoming lines with $W_{new} < W_{min} - K$ ($K=1$) bypass cache insertion, preventing cache pollution and reducing insertion energy by nearly 30%.
  - **Hardware Metadata:** 3 bits for weight + 2 bits for SRRIP RRPV per entry; 2 slots $\times$ 3 bits pitfall detector per set (only 46 bits overhead per 8-way set $\approx 1\%$).

---

## 2. Files Created and Modified in gem5

| Path | Purpose |
|---|---|
| [`src/mem/cache/replacement_policies/furbys_rp.hh`](file:///c:/Users/JAYESH/Desktop/jayesh/textbooks/comp_arch/proj/gem5/src/mem/cache/replacement_policies/furbys_rp.hh) | C++ header declaring `FURBYSRP`, `FURBYSReplData`, `LocalPitfallDetector`, and selective bypassing logic. |
| [`src/mem/cache/replacement_policies/furbys_rp.cc`](file:///c:/Users/JAYESH/Desktop/jayesh/textbooks/comp_arch/proj/gem5/src/mem/cache/replacement_policies/furbys_rp.cc) | C++ implementation of min-weight selection, SRRIP degradation fallback, selective bypassing, and profile loading. |
| [`src/mem/cache/replacement_policies/flack_rp.hh`](file:///c:/Users/JAYESH/Desktop/jayesh/textbooks/comp_arch/proj/gem5/src/mem/cache/replacement_policies/flack_rp.hh) | C++ header declaring `FLACKRP` offline variable-cost near-optimal policy. |
| [`src/mem/cache/replacement_policies/flack_rp.cc`](file:///c:/Users/JAYESH/Desktop/jayesh/textbooks/comp_arch/proj/gem5/src/mem/cache/replacement_policies/flack_rp.cc) | C++ implementation of FLACK cost/size metric, partial hit handling, and trace tracking. |
| [`src/mem/cache/replacement_policies/furbys_rp.test.cc`](file:///c:/Users/JAYESH/Desktop/jayesh/textbooks/comp_arch/proj/gem5/src/mem/cache/replacement_policies/furbys_rp.test.cc) | gem5 GTest unit test suite covering entry instantiation, touch/reset, min-weight selection, bypassing, and pitfall degradation. |
| [`src/mem/cache/replacement_policies/ReplacementPolicies.py`](file:///c:/Users/JAYESH/Desktop/jayesh/textbooks/comp_arch/proj/gem5/src/mem/cache/replacement_policies/ReplacementPolicies.py) | Registered `FURBYSRP` and `FLACKRP` SimObjects with Python parameters. |
| [`src/mem/cache/replacement_policies/SConscript`](file:///c:/Users/JAYESH/Desktop/jayesh/textbooks/comp_arch/proj/gem5/src/mem/cache/replacement_policies/SConscript) | Registered C++ source files and unit tests into gem5's build system. |
| [`configs/example/furbys_uop_cache.py`](file:///c:/Users/JAYESH/Desktop/jayesh/textbooks/comp_arch/proj/gem5/configs/example/furbys_uop_cache.py) | Example gem5 configuration script demonstrating how to instantiate and connect `FURBYSRP` to an O3CPU model. |
| [`util/furbys/jenks_breaks.py`](file:///c:/Users/JAYESH/Desktop/jayesh/textbooks/comp_arch/proj/gem5/util/furbys/jenks_breaks.py) | Offline Fisher-Jenks dynamic programming clustering to partition hit rates into 8 optimal classes (0-7). |
| [`util/furbys/flack_sim.py`](file:///c:/Users/JAYESH/Desktop/jayesh/textbooks/comp_arch/proj/gem5/util/furbys/flack_sim.py) | Offline FLACK simulator incorporating variable uop cost, partial hits, and lazy eviction. |
| [`util/furbys/eval_uop_cache.py`](file:///c:/Users/JAYESH/Desktop/jayesh/textbooks/comp_arch/proj/gem5/util/furbys/eval_uop_cache.py) | End-to-end evaluation runner comparing LRU, SRRIP, GHRP, FLACK, and FURBYS over data center workloads. |
| [`util/furbys/test_furbys_standalone.cpp`](file:///c:/Users/JAYESH/Desktop/jayesh/textbooks/comp_arch/proj/gem5/util/furbys/test_furbys_standalone.cpp) | Standalone C++ test program testing all replacement mechanics. |

---

## 3. How to Run

### Step 1: Run the End-to-End Evaluation
Run the python evaluation suite to simulate the 512-entry 8-way micro-op cache across data center workloads:
```bash
python gem5/util/furbys/eval_uop_cache.py
```

### Step 2: Run the Jenks Natural Breaks Profiler
Test the Fisher-Jenks clustering algorithm:
```bash
python gem5/util/furbys/jenks_breaks.py
```

### Step 3: Run the Offline FLACK Simulator
Test the offline near-optimal replacement solver:
```bash
python gem5/util/furbys/flack_sim.py
```

### Step 4: Run the Standalone C++ Verification
Compile and run the C++ unit test runner:
```powershell
cmd.exe /c "`"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat`" && cl /std:c++17 /EHsc /Fegem5\util\furbys\test_furbys.exe gem5\util\furbys\test_furbys_standalone.cpp && gem5\util\furbys\test_furbys.exe"
```
Or run the precompiled binary:
```powershell
./gem5/util/furbys/test_furbys.exe
```

### Step 5: Building gem5 with FURBYS and FLACK
To build inside a Linux/Docker environment or with scons:
```bash
scons build/X86/gem5.opt -j$(nproc)
```
And execute with the example configuration:
```bash
./build/X86/gem5.opt configs/example/furbys_uop_cache.py --policy=FURBYS
```

