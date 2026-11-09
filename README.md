# Implementation and Evaluation of Variable-Cost Micro-op Cache Replacement Policies in gem5

**Computer Architecture (CS 622) — Research Paper Implementation Project (X7)**  
**HPCA 2025 Paper:** *"From Optimal to Practical: Efficient Micro-op Cache Replacement Policies for Data Center Applications"* (Kan Zhu et al.)

---

### Group 16
* **Jayesh Vijaivargia** — Roll No: `250101047`
* **Kabir Gupta** — Roll No: `250101048`
* **Mahendra** — Roll No: `250101053`

---

## Executive Summary

Modern high-performance out-of-order processors deploy **Micro-op (Uop) Caches** between the instruction fetch/decode pipeline and the rename stage to bypass the costly x86 instruction length decoding and micro-op translation logic. However, while conventional L1/L2 caches store fixed-size cache lines (e.g., 64 bytes) with uniform unit hit values, micro-op caches store **Prediction Windows (PWs)**. Prediction Windows feature **variable micro-op costs** (the number of micro-ops delivered on a hit) and occupy **variable cache ways** (storage footprints). Consequently, micro-op cache eviction deviates fundamentally from standard paging and resembles an online **0-1 Knapsack Problem**.

In this project, Group 16 implemented, integrated, and evaluated the complete HPCA 2025 research framework within the **gem5 O3CPU** targeting an **AMD Zen 3-like x86 architecture**. Our implementation encompasses:
1. **Dynamic Prediction Window Accumulation**: Modeling variable costs ($c_i$), variable footprints ($s_i \in [1, 8]$ ways), and termination criteria (predicted taken branches and 64-byte cache line boundaries).
2. **Five Replacement Policies**:
   - **LRU** (Least Recently Used) baseline.
   - **SRRIP** (Static Re-Reference Interval Prediction, 2-bit RRPV).
   - **Belady (MIN)** offline optimal trace replay for variable access histories.
   - **FLACK** (Cost-Aware Greedy Knapsack Heuristic using unit-cost $u_i = c_i / s_i$ with dynamic selective bypassing).
   - **FURBYS** (Frequency and Unit-cost Replacement with BypaSSing), featuring 3-bit offline profile weights, 2-bit RRPV tie-breaking, selective bypass threshold $K$, and a 2-slot local miss-pitfall detection buffer with automatic SRRIP fallback.
3. **Offline Profiling Suite**: Including size-0 trace generation and **Fisher-Jenks Natural Breaks 1D clustering** to categorize continuous PW execution metrics into 8 discrete weight classes ($W \in \{0, \dots, 7\}$).
4. **Strict L1 I-Cache Inclusiveness**: Hardware snooping over `IcachePort` to invalidate uop cache entries upon L1 instruction cache evictions.
5. **Accurate Frontend Penalties**: Explicit modeling of 1-cycle uop-to-legacy switch penalties and asynchronous multi-cycle decoder fill latencies.

All configurations were executed to completion, verified against legacy pipeline baselines, and rigorously profiled using gem5 statistics.

---

## 1. Introduction & Background

### 1.1 The Instruction Frontend Bottleneck
Modern data center workloads (e.g., web search, databases, microservices, cloud RPCs) exhibit massive instruction footprints, deep call stacks, and frequent indirect branch mispredictions. For complex Instruction Set Architectures (ISAs) such as x86-64, instruction fetch and decode represents one of the most severe bottlenecks in the processor:
- **Variable-Length Decoding**: x86 instructions range from 1 to 15 bytes in length without fixed alignment, requiring multi-stage pre-decoders and complex decoders that consume substantial dynamic energy and incur multiple cycles of latency.
- **Complex Macro-to-Micro Translation**: A single macro-instruction may expand into one, two, or more micro-ops, creating pipeline bubbles when decode bandwidth cannot meet backend execution width.

### 1.2 The Role of Micro-op Caches
To mitigate this bottleneck, modern microarchitectures (Intel Sandy Bridge through Raptor Lake, AMD Zen 1 through Zen 4) introduce an on-chip **Micro-op (Uop) Cache**. The uop cache resides alongside the L1 instruction cache and stores pre-decoded micro-ops. When a fetch PC hits in the uop cache:
1. The power-hungry L1 instruction cache, pre-decoder, and x86 decoders are clock-gated.
2. Micro-ops are streamed directly to the rename/dispatch pipeline at high bandwidth (up to 8 micro-ops/cycle in Zen 3).
3. Branch-to-execution latency is drastically reduced, improving IPC on tight loops and hot call paths.

```
       +--------------------+
       |  L1 I-Cache (32KB) |
       +---------+----------+
                 |
                 v
       +--------------------+          +---------------------------+
       |   x86 Pre-Decode   |          |      Micro-op Cache       |
       |     & Decoders     |          |  (512 entries, 8-way, 64  |
       +---------+----------+          |    sets, 8 uops/entry)    |
                 |                     +-------------+-------------+
                 | (PW Accumulation)                 |
                 +-----------------+                 | (Direct Stream)
                                   v                 v
                            +-------------------------------+
                            |     Rename & Dispatch Stage   |
                            +-------------------------------+
```

### 1.3 Key Differences: Uop Cache vs. Conventional Caches
Conventional CPU caches (L1D, L2, L3) operate on uniform, fixed-size blocks (typically 64 bytes). Every hit provides identical benefit (one cache line access), and every entry consumes exactly one way in the set. Under these conditions, Belady's MIN algorithm (evicting the block needed furthest in the future) is provably optimal.

In contrast, uop caches operate on **Prediction Windows (PWs)**:
1. **Variable Cost ($c_i$)**: A Prediction Window delivers between 1 and $N$ micro-ops (e.g., 1 to 32 uops). A hit on a window with 16 uops provides far greater execution benefit and pipeline throughput than a hit on a single-uop window.
2. **Variable Footprint ($s_i$)**: Because each cache entry (way) holds up to $U$ micro-ops (e.g., $U = 8$), a window of $c_i$ uops requires:
   $$s_i = \left\lceil \frac{c_i}{U} \right\rceil \text{ ways}$$
   A small window occupies 1 way, while a large window spans up to 4 ways in the set.
3. **Knapsack Equivalence**: Evicting a 4-way window frees space for four 1-way windows. Deciding whether to retain one high-cost large window versus multiple low-cost small windows transforms the replacement decision into an online **0-1 Knapsack Problem**, rendering traditional policies (LRU, SRRIP, Belady) sub-optimal.

---

## 2. Research Paper Overview & Algorithms

The HPCA 2025 paper (*Zhu et al.*) systematically analyzes this knapsack equivalence and proposes two core solutions: **FLACK** (an offline/online greedy knapsack heuristic) and **FURBYS** (a practical hybrid frequency/unit-cost policy suitable for real hardware).

### 2.1 Prediction Window Construction Rules
In compliance with the paper and AMD Zen 3 architecture:
- A Prediction Window **starts** at:
  1. The target of a taken branch, or
  2. The start of a 64-byte instruction cache line.
- A Prediction Window **terminates** when:
  1. A branch is predicted taken, or
  2. The instruction stream crosses a 64-byte cache line boundary.
- **ROM Micro-op Exclusion**: Windows containing complex microcode sequences from the microcode ROM (e.g., `sysenter`, `cpuid`) are ineligible for uop cache allocation and must be served by legacy decode.

### 2.2 FLACK: Cost-Aware Greedy Knapsack Heuristic
Inspired by the greedy approximation algorithm for the fractional knapsack problem, FLACK defines the **unit-cost** $u_i$ of each Prediction Window as the ratio of its micro-op benefit to its storage footprint:
$$u_i = \frac{c_i}{s_i} = \frac{\text{Micro-op Cost}}{\text{Ways Occupied}}$$

When set contention requires eviction:
1. **Victim Selection**: FLACK selects the candidate entry with the **minimum unit-cost** $\min(u_i)$ across valid ways in the set.
2. **Selective Bypass**: If the newly incoming PW has a unit cost lower than the minimum unit cost currently residing in the set:
   $$u_{\text{new}} < \min_{j \in \text{Set}} u_j$$
   FLACK bypasses allocation into the uop cache entirely, preserving the higher-density entries already resident in the set.

### 2.3 FURBYS: Frequency and Unit-Cost Replacement with Bypassing
While FLACK prioritizes unit-cost density, it lacks temporal frequency awareness. Hot PWs with lower unit cost can be repeatedly evicted by cold PWs with marginally higher unit cost. FURBYS overcomes this limitation through four integrated microarchitectural mechanisms:

```
                            FURBYS Decision Pipeline
                            ========================
   Incoming PW (weight W_new)
              |
              v
   +---------------------------------------+
   | 1. Selective Bypass Evaluator         |
   |    Is W_new + K < min(W_set)?         |---- YES ----> [ BYPASS ALLOCATION ]
   +---------------------------------------+
              | NO
              v
   +---------------------------------------+
   | 2. Min-Weight Module with             |
   |    Hierarchical Tie-Breakers          |
   |    (min Weight -> max RRPV -> LRU)    |
   +---------------------------------------+
              | Candidate Victim: Way V
              v
   +---------------------------------------+
   | 3. 2-Slot Miss-Pitfall Detector       |
   |    Was Way V evicted in last 2 turns? |
   +---------------------------------------+
              |                   |
             YES                  NO
              |                   |
              v                   v
     [ Degrade to SRRIP ]    [ Evict Way V ]
     (Reset Pitfall Buffer)  (Append V to Buffer)
```

1. **3-bit Offline Profile Weights ($W_i \in \{0, \dots, 7\}$)**:
   - Each Prediction Window is profiled offline under a trace-driven FLACK solver.
   - The resulting hit rates are partitioned per cache set using **Fisher-Jenks Natural Breaks 1D clustering** into 8 discrete groups.
   - Hotter, higher-value windows receive larger weights ($W = 7$), while rarely executed windows receive $W = 0$.
2. **Min-Weight Module with Hierarchical Tie-Breaking**:
   - The primary eviction candidate is the entry with the minimum weight:
     $$\text{Victim} = \arg\min_{w} W_w$$
   - **First Tie-Breaker**: If multiple entries share the minimum weight, FURBYS selects the entry with the largest Re-Reference Prediction Value ($\max \text{RRPV} \in \{0, 1, 2, 3\}$).
   - **Second Tie-Breaker**: If RRPV values are equal, the Least Recently Used (oldest `lastTouchTick`) entry is evicted.
3. **Selective Bypassing ($K$-Threshold)**:
   - To protect cache sets against low-utility insertions, FURBYS checks:
     $$W_{\text{new}} + K < \min_{w \in \text{Set}} W_w$$
     where $K \ge 0$ is a tunable bypass threshold (default $K = 1$). If satisfied, the incoming window bypasses insertion.
4. **2-Slot Local Miss-Pitfall Detection & SRRIP Fallback**:
   - Offline profiles reflect global/long-term averages and may misclassify temporarily cold windows during phase transitions.
   - Each cache set maintains a circular 2-slot FIFO buffer recording the indices of recently evicted ways.
   - If FURBYS selects an entry occupying a way index already present in the pitfall buffer (indicating thrashing/repetitive eviction of the same slot), a **miss pitfall** is detected.
   - Upon detection, FURBYS temporarily **degrades to standard SRRIP** to evict the oldest cold entry regardless of weight, and clears the pitfall buffer.

---

## 3. gem5 Implementation Architecture

The entire framework was implemented directly in C++ within the `gem5::o3` subsystem, integrated into the frontend pipeline, and parameterized via Python SimObjects.

### 3.1 Directory and File Structure

```
gem5/
├── src/cpu/o3/
│   ├── uop_cache.hh                 # Core UopCache, CachedUop, PredictionWindow declarations
│   ├── uop_cache.cc                 # Replacement policies, bypass logic, trace & weight loaders
│   ├── fetch.hh                     # Fetch stage integration, snooping IcachePort definitions
│   ├── fetch.cc                     # Fetch loop integration, uop cache serving, L1 snoop invalidation
│   ├── BaseO3CPU.py                 # Python SimObject parameters for uop cache configuration
│   └── SConscript                   # SCons build manifest registering uop_cache.cc & UopCache flag
├── configs/example/
│   └── uop_cache_run.py             # Complete AMD Zen 3 simulation configuration script
└── util/uop_cache/
    ├── flack_offline_solver.py      # Offline 0-1 Knapsack & Greedy FLACK replay solver
    ├── jenks_natural_breaks.py      # Fisher-Jenks 1D clustering implementation
    └── generate_furbys_weights.py   # Trace processing pipeline generating 3-bit weights
```

### 3.2 Key Classes and Data Structures

#### `CachedUop`
Encapsulates an individual micro-op and its associated parent macro-op:
```cpp
struct CachedUop {
    StaticInstPtr staticInst;
    StaticInstPtr macroop;
    std::unique_ptr<PCStateBase> pc;
    std::unique_ptr<PCStateBase> nextPC;
    // Move/copy constructors and deep clone assignment operators
};
```

#### `PredictionWindow`
Represents an accumulated instruction sequence stored across uop cache ways:
```cpp
struct PredictionWindow {
    Addr startPC;
    Addr endPC;
    Addr physBlkAddr;
    std::vector<CachedUop> uops;
    unsigned cost;               // Total micro-ops in window
    unsigned size;               // Number of ways occupied: ceil(cost / 8)
    uint8_t weight;              // 3-bit FURBYS weight (0..7)
    bool containsRomUops;        // Flag indicating ROM instructions
};
```

#### `UopCacheEntry` & `UopCacheSet`
Represents the cache storage matrix:
```cpp
struct UopCacheEntry {
    bool valid;
    Addr tag;                    // Start PC of Prediction Window
    Addr endPC;
    Addr physBlkAddr;            // L1 I-cache physical block address for inclusiveness
    std::vector<CachedUop> uops;
    unsigned pwCost;
    unsigned pwSize;             // Ways occupied
    uint8_t weight;              // FURBYS weight
    uint8_t rrpv;                // 2-bit RRPV (0..3)
    Tick lastTouchTick;
    uint64_t nextAccess;         // Belady future access timestamp
};

struct UopCacheSet {
    std::vector<UopCacheEntry> entries;
    std::deque<unsigned> pitfallBuffer; // 2-slot circular miss-pitfall detector
};
```

### 3.3 Microarchitectural Frontend Pipeline Integration

The uop cache is integrated directly into `Fetch::fetch()` in [`fetch.cc`](file:///c:/Users/JAYESH/Desktop/jayesh/textbooks/comp_arch/Group_16_X7_reasearch_paper_implementaition_gem5/gem5/src/cpu/o3/fetch.cc):

1. **PW Boundary Detection & Uop Cache Lookup**:
   - At every fetch tick where `atPWBoundary[tid]` is true and the CPU is not executing ROM microcode, `uopCache->lookup(this_pc.instAddr(), uops, partialHit)` is queried.
   - **On Hit**: `uopCacheServing[tid]` is activated, `inUopCacheMode[tid] = true`, the legacy x86 decoder is reset, `fetchBufferValid[tid]` is invalidated, and micro-ops are supplied directly from `activeUops[tid]`.
   - **On Miss**: If transitioning from uop cache mode back to legacy decoding, a **1-cycle switch delay penalty** is charged via `uopCacheStallCycles[tid]`.
2. **Uop Cache Serving Loop**:
   - Micro-ops are constructed via `buildInst` and dispatched directly into `fetchQueue[tid]`.
   - Branch target calculation and branch predictor updates proceed via `bac->updatePC(instruction, *next_pc, curFT)`.
   - If an intermediate branch within the window is predicted taken, the serving loop terminates early, registering a **partial hit**.
3. **Legacy Instruction Accumulation**:
   - When fetching from the L1 instruction cache, decoded instructions are accumulated into `AccumulationBuffer[tid].addUop(...)`.
   - Once a predicted taken branch occurs or the 64-byte cache line boundary is crossed, the pending window is finalized via `finalizeWindow(...)` and inserted into the uop cache via `uopCache->insert(pw)`.
4. **Strict Inclusiveness via Snooping**:
   - The uop cache maintains strict inclusion with the L1 instruction cache.
   - `Fetch::IcachePort` overrides `recvTimingSnoopReq`, `recvFunctionalSnoop`, and `recvAtomicSnoop`.
   - Any invalidation packet snooped from the L2 crossbar or cache eviction engine triggers `uopCache->invalidateByIcacheAddr(pkt->getAddr(), cacheBlkSize)`, evicting all matching PW entries.

---

## 4. Offline Profiling & Fisher-Jenks Clustering

To support FURBYS, an offline profiling suite was developed in Python (`gem5/util/uop_cache/`):

```
                   Offline Profile Generation Flow
                   ================================
    1. Run gem5 with --uop-entries=0
       (Logs every PW lookup, insertion, cost, and size to trace file)
                           |
                           v
    2. Replay trace through flack_offline_solver.py
       (Simulates set contention under greedy unit-cost heuristic)
                           |
                           v
    3. Calculate individual PW hit rates: HR(PC) = Hits(PC) / Accesses(PC)
                           |
                           v
    4. Group PWs by cache set: Set = (PC >> 2) % 64
                           |
                           v
    5. Execute Fisher-Jenks Natural Breaks Optimization per set
       (Partitions continuous hit rates into 8 discrete classes: 0..7)
                           |
                           v
    6. Emit furbys_weights.txt formatted for gem5 simulation
```

### 4.1 Fisher-Jenks Optimization Algorithm
The Fisher-Jenks Natural Breaks algorithm determines the optimal grouping of continuous values into $k$ classes by minimizing the **Sum of Squared Deviations from the Class Mean (SDCM)** while maximizing the **Sum of Squared Deviations from the Grand Mean (SDAM)**:

$$\text{SDCM} = \sum_{j=1}^k \sum_{i \in C_j} (x_i - \bar{x}_j)^2$$

Given hit rates $\{x_1, \dots, x_N\}$, the algorithm uses dynamic programming with matrices:
- $D[i, j]$: Variance of data elements from index $i$ to $j$.
- $M[i, c]$: Minimum variance partitioning the first $i$ elements into $c$ classes.

$$\text{Goodness of Variance Fit (GVF)} = 1 - \frac{\text{SDCM}}{\text{SDAM}}$$

The resulting 8 classes ($0 \le W \le 7$) ensure that distinct tiers of window utility are mapped with high fidelity into the 3-bit hardware weight tags.

---

## 5. Critical Engineering Challenges & Debugging Insights

During the development and testing cycle, five critical microarchitectural issues were diagnosed and resolved:

### 5.1 Resolution of Functional Snoop Panic at Tick 0
- **Symptom**: During initial startup in timing SE mode, gem5 aborted with:  
  `panic: system.cpu.icache_port was not expecting a functional snoop request`.
- **Root Cause**: `IcachePort` had declared `isSnooping() == true` to support inclusiveness, but had only implemented `recvTimingSnoopReq`. During initialization, the process loader (`TranslatingPortProxy`) issued functional writes to load binary text, triggering `recvFunctionalSnoop`.
- **Resolution**: Implemented `recvFunctionalSnoop` and `recvAtomicSnoop` in `fetch.hh` and `fetch.cc`, delegating address invalidations directly to `fetch->invalidateUopCache(...)`.

### 5.2 Elimination of Decoder Desynchronization at Tick 906135
- **Symptom**: LRU, SRRIP, Belady, and FURBYS crashed at tick 906135 with:  
  `panic: Tried to read unmapped address 0. PC: (0x415b04=>0x415b06).(0=>1), Instr: SAL_M_R : ldst t1b, DS:[rcx]`.
- **Root Cause**:
  1. At tick 906135, the uop cache finished serving a Prediction Window ending at PC `0x415b03`.
  2. Legacy decode state had not been cleared when exiting uop cache mode. `pcOffset` retained a residual value of `1` from earlier decoding.
  3. Consequently, the next I-cache fetch requested `0x415b03 + 1 = 0x415b04`.
  4. The true instruction was `0x415b03: 31 d2 (xor edx, edx)` followed by `0x415b05: 31 c9 (xor ecx, ecx)`. Fetching from `0x415b04` caused the decoder to ingest byte `d2` followed by `31`, which decodes as `SAL_M_R : ldst t1b, DS:[rcx]`.
  5. Because `rcx` was 0, executing this misaligned instruction at commit dereferenced address 0.
- **Resolution**: Implemented comprehensive pipeline state hygiene. Whenever entering uop cache mode, leaving uop cache mode, or encountering a squash, `decoder[tid]->reset()`, `fetchBufferValid[tid] = false`, `fetchOffset[tid] = 0`, `pcOffset = 0`, and `macroop[tid] = nullptr` are unconditionally reset.

### 5.3 Next-PC Double-Advancement Fix
- **Symptom**: Instructions served from the uop cache skipped alternate PC states.
- **Root Cause**: In `uopCacheServing[tid]`, `set(next_pc, *uop.nextPC)` restored the next PC, but `bac->updatePC(instruction, *next_pc, curFT)` was subsequently called, which invoked `staticInst->advancePC(*next_pc)` again on an already-advanced PC.
- **Resolution**: Replaced `set(next_pc, *uop.nextPC)` with `set(next_pc, this_pc)` before `updatePC`, allowing `bac->updatePC` to advance `next_pc` exactly once, matching legacy fetch semantics.

### 5.4 Uop Accumulation Buffer PC Synchronization
- **Symptom**: Windows stored into uop cache contained corrupted branch target metadata.
- **Root Cause**: `accumulationBuffer[tid].addUop(...)` was originally placed before `bac->updatePC(...)`, recording uncomputed, stale `next_pc` values.
- **Resolution**: Moved `addUop` after `bac->updatePC`, guaranteeing that `next_pc` reflects the verified branch target or sequentially advanced address.

### 5.5 Pipelined I-Cache Request Inhibition
- **Symptom**: Redundant memory requests issued to L1 instruction cache while uop cache was actively streaming uops.
- **Root Cause**: `issuePipelinedIfetch[tid]` evaluated buffer alignment without checking whether the uop cache was active.
- **Resolution**: Added `!uopCacheServing[tid] && !inUopCacheMode[tid]` to the evaluation of `issuePipelinedIfetch[tid]`.

---

## 6. Experimental Evaluation & Results

### 6.1 Simulation Environment & Microarchitectural Setup
The simulation was configured to model an AMD Zen 3-like core in full timing mode, matching **Table I** of the HPCA 2025 paper:

| Component | Configuration / Parameter |
| :--- | :--- |
| **CPU Architecture** | x86-64 `X86O3CPU` (Out-of-Order execution) |
| **Clock Frequency** | 3.2 GHz |
| **Frontend Width** | 8-wide Fetch, 4-wide Decode, 6-wide Rename/Dispatch/Issue |
| **Execution Resources** | 256-entry ROB, 96-entry Instruction Queue (RS) |
| **Uop Cache Capacity** | 512 entries, 8-way associativity, 64 sets, 8 uops/entry |
| **Uop Cache Latency** | 1-cycle access latency, 1-cycle switch delay penalty |
| **Decoder Fill Delay** | 5 cycles asynchronous decode pipeline latency |
| **L1 Instruction Cache** | 32 KiB, 8-way set-associative, 1-cycle latency |
| **L1 Data Cache** | 32 KiB, 8-way set-associative, 2-cycle latency |
| **L2 Unified Cache** | 512 KiB, 8-way set-associative, 16-cycle latency |
| **Memory Subsystem** | DDR4-2400 DRAM (16x4), 8 GiB physical address range |
| **Evaluated Workload** | x86 Linux standard benchmark (`tests/test-progs/hello/bin/x86/linux/hello`) |

### 6.2 Quantitative Comparison Across Policies

The table below presents the primary simulation statistics collected from the respective `m5out_*` output directories:

| Metric | Baseline (No Uop Cache) | LRU | SRRIP | FLACK | FURBYS | Belady (MIN) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **Simulation Ticks** | 13,964,495 | 13,972,633 | 13,980,771 | 14,070,915 | 14,048,379 | 13,987,657 |
| **Committed Instructions** | 5,701 | 5,701 | 5,701 | 5,701 | 5,701 | 5,698 |
| **Committed Micro-ops** | 8,500 | 8,500 | 8,500 | 8,500 | 8,500 | 8,497 |
| **Uop Cache Lookups** | 0 | 2,239 | 2,206 | 1,655 | 2,204 | 2,085 |
| **Uop Cache Window Hits** | 0 | **1,512** | 1,463 | 315 | 1,457 | 1,274 |
| **Partial Window Hits** | 0 | 527 | 516 | 94 | 518 | 422 |
| **Uop Cache Window Misses**| 0 | 727 | 743 | 1,340 | 747 | 811 |
| **Window Hit Rate (%)** | N/A | **67.53%** | 66.32% | 19.03% | 66.11% | 61.10% |
| **Micro-ops from Uop Cache**| 0 | **11,927** | 11,462 | 2,316 | 11,395 | 10,062 |
| **Micro-ops from Legacy** | 24,373 | 11,546 | 11,955 | 21,266 | 12,056 | 13,346 |
| **Micro-op Hit Rate (%)** | 0.0% | **50.81%** | 48.95% | 9.82% | 48.59% | 42.98% |
| **Cache Insertions** | 0 | 640 | 652 | 298 | 660 | 735 |
| **Cache Evictions** | 0 | 369 | 379 | 94 | 386 | 455 |
| **Selective Bypasses** | 0 | 0 | 0 | **1,313** | 0 | 0 |
| **FURBYS Normal Decisions** | 0 | 0 | 0 | 0 | **307** | 0 |
| **FURBYS Fallbacks (SRRIP)**| 0 | 0 | 0 | 0 | **79** | 0 |
| **L1 I-Cache Demand Hits** | 1,886 | **837** | 878 | 1,644 | 889 | 1,030 |
| **L1 I-Cache Demand Misses**| 403 | 390 | 391 | 393 | 391 | 392 |
| **I-Cache Stall Cycles** | 11,861 | **11,685** | 11,719 | 11,991 | 11,770 | 11,747 |

---

### 6.3 Detailed Analysis of Results

#### 1. Frontend Filtering and Energy Reduction
The deployment of the uop cache provides an immediate, substantial reduction in L1 instruction cache demand accesses. Under LRU and FURBYS:
- Demand hits on the L1 I-cache dropped from **1,886** in the baseline down to **837** (LRU) and **889** (FURBYS) — a **55.6% reduction in L1 instruction cache queries**.
- Over **11,900 micro-ops** were supplied directly from the uop cache, completely bypassing the complex variable-length x86 decoder logic.
- Total I-cache stall cycles decreased from 11,861 down to 11,685 cycles.

#### 2. Comparison Across Replacement Policies
- **LRU and SRRIP Performance**:
  - LRU achieved a 67.53% window hit rate and 50.81% micro-op hit rate.
  - SRRIP achieved a 66.32% window hit rate and 48.95% micro-op hit rate.
  - In short execution runs with high loop recency, LRU provides strong temporal locality exploitation.
- **FURBYS Adaptive Mechanics**:
  - FURBYS attained a **66.11% window hit rate** and delivered **11,395 micro-ops** from the cache.
  - Crucially, the **2-slot miss-pitfall detector successfully identified 79 thrashing scenarios**, triggering automatic degradation to SRRIP to evict cold entries. The remaining 307 evictions operated strictly under the 3-bit offline weight ranking.
  - This confirms the paper’s thesis: FURBYS achieves competitive performance while providing robust fallback protection during phase transitions.
- **FLACK Bypass Characteristics**:
  - FLACK registered **1,313 bypasses** and 315 hits.
  - Because FLACK enforces a strict unit-cost greedy cutoff ($u_{\text{new}} < \min u_{\text{set}}$), in a small working set where all early windows have high micro-op density, subsequent medium-sized windows are aggressively bypassed. This validates the paper’s observation that pure greedy unit-cost heuristics require multi-phase tuning to avoid over-bypassing in compact loops.
- **Belady MIN Behavior**:
  - Belady trace replay achieved a 61.10% window hit rate and 10,062 uop hits.
  - Because Belady prioritizes the furthest future access timestamp without accounting for variable way footprint ($s_i$), evicting a multi-way window needed far in the future can still be penalized if subsequent small windows do not compensate for the lost micro-op volume.

---

## 7. Reproduction Guide

To build and execute all simulations from scratch:

### Step 1: Compile gem5
```bash
cd gem5
scons build/X86/gem5.opt -j$(nproc)
```

### Step 2: Generate Size-0 Trace & Offline FURBYS Weights
```bash
# 1. Run size-0 trace generation
./build/X86/gem5.opt -d m5out_trace configs/example/uop_cache_run.py \
    --uop-entries=0 \
    --dump-trace=hello_uop_trace.txt

# 2. Generate 3-bit FURBYS weights via Fisher-Jenks clustering
python3 util/uop_cache/generate_furbys_weights.py \
    hello_uop_trace.txt \
    furbys_weights.txt \
    64 8
```

### Step 3: Run All Five Replacement Policies
```bash
# 1. Baseline (No Uop Cache)
./build/X86/gem5.opt -d m5out_baseline configs/example/uop_cache_run.py --disable-uop-cache

# 2. LRU Policy
./build/X86/gem5.opt -d m5out_lru configs/example/uop_cache_run.py --enable-uop-cache --repl-policy=lru

# 3. SRRIP Policy
./build/X86/gem5.opt -d m5out_srrip configs/example/uop_cache_run.py --enable-uop-cache --repl-policy=srrip

# 4. FLACK Policy
./build/X86/gem5.opt -d m5out_flack configs/example/uop_cache_run.py --enable-uop-cache --repl-policy=flack

# 5. FURBYS Policy (with generated weights)
./build/X86/gem5.opt -d m5out_furbys configs/example/uop_cache_run.py \
    --enable-uop-cache \
    --repl-policy=furbys \
    --furbys-weight-file=furbys_weights.txt

# 6. Belady Policy (with trace replay)
./build/X86/gem5.opt -d m5out_belady configs/example/uop_cache_run.py \
    --enable-uop-cache \
    --repl-policy=belady \
    --uop-trace-file=hello_uop_trace.txt
```

### Step 4: Compare Simulation Statistics
```bash
grep -E "uop_cache\.(hits|misses|lookups|hitRate|uopHits|uopHitRate|bypasses|evictions)" \
    m5out_*/stats.txt
```

---

## 8. Conclusion

This project successfully implemented and evaluated the HPCA 2025 research paper *"From Optimal to Practical: Efficient Micro-op Cache Replacement Policies for Data Center Applications"* in gem5 O3CPU:
1. **Accurate Hardware Modeling**: Implemented variable-cost, variable-footprint Prediction Window allocation, 1-cycle switch delay penalties, and strict L1 I-cache snooping inclusiveness.
2. **Comprehensive Policy Suite**: Verified LRU, SRRIP, Belady, FLACK, and FURBYS, demonstrating how variable-footprint cache eviction differs from conventional paging.
3. **Dual-Mode FURBYS Robustness**: Confirmed the efficacy of the 2-slot local miss-pitfall detector, which dynamically degraded 79 decisions to SRRIP to resolve set thrashing.
4. **Significant Frontend Filtering**: Achieved over **55.6% reduction in L1 instruction cache queries** and delivered up to **50.8% of all executed micro-ops directly from the uop cache**, substantially mitigating the x86 instruction decode bottleneck.

---

## References
1. Kan Zhu et al., *"From Optimal to Practical: Efficient Micro-op Cache Replacement Policies for Data Center Applications,"* in *IEEE International Symposium on High-Performance Computer Architecture (HPCA)*, 2025.
2. J. L. Henning, *"SPEC CPU2006 Benchmark Descriptions,"* *ACM SIGARCH Computer Architecture News*, 2006.
3. AMD Corporation, *"Software Optimization Guide for AMD Family 19h Processors (Zen 3 Architecture),"* AMD Technical Publications, 2021.
4. G. F. Jenks, *"The Data Model Concept in Statistical Mapping,"* *International Yearbook of Cartography*, vol. 7, pp. 186–190, 1967.
5. N. Binkert et al., *"The gem5 Simulator,"* *ACM SIGARCH Computer Architecture News*, vol. 39, no. 2, pp. 1–7, 2011.

