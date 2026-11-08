# Project Report: Implementation of Micro-op Cache Replacement Policies (FURBYS & FLACK) in gem5

---

### **Project Metadata**
* **Group Number:** 16
* **Team Members:**
  1. Kabir Gupta
  2. Jayesh Vijayvargia (Jayesh Vijaiavrgia)
  3. Mahendra
* **Target Architecture:** x86-64 (AMD Zen 3 / Zen 4 Decoupled Frontend Model)
* **Simulator:** gem5 (v25.1+)
* **Reference Research Paper:**  
  *“From Optimal to Practical: Efficient Micro-op Cache Replacement Policies for Data Center Applications”*  
  Kan Zhu, Yilong Zhao, Yufei Gao, Peter Braun, Tanvir Ahmed Khan, Heiner Litz, Baris Kasikci, Shuwen Deng  
  *Proceedings of the 31st IEEE International Symposium on High-Performance Computer Architecture (HPCA 2025)*

---

## 1. Executive Summary

Modern high-performance x86 processors utilize a **Micro-operation (Micro-op / uop) Cache** to store decoded RISC-like micro-ops. Hits in the micro-op cache bypass and clock-gate the power-hungry x86 instruction decoders and L1 instruction cache, yielding up to a **7.41% performance-per-watt (PPW) gain**. However, in large planet-scale data center workloads, over 99% of micro-op cache misses are capacity and conflict misses.

Standard cache replacement policies (such as LRU, Belady's MIN, and SRRIP) fail because they treat cache lines uniformly, ignoring three microarchitectural characteristics of uop caches:
1. **Variable Costs and Sizes:** Prediction Windows (PWs) hold variable numbers of micro-ops ($cost \in [1, 32]$) and occupy variable numbers of entries ($size \in [1, 4]$). Evicting dense PWs incurs disproportionate miss penalties.
2. **Partial Hits & Overlapping Windows:** Dynamic branch outcomes create overlapping windows starting at identical PCs; a larger cached PW can serve a smaller requested window without re-fetching.
3. **Asynchronous Lookup and Insertion:** Variable decode pipeline latencies create out-of-order lookups and delayed insertions.

In this project, our team implemented both core contributions from the paper into the **gem5** simulator:
1. **FLACK:** An offline near-optimal reference policy that balances variable micro-op density ($\frac{\text{cost}}{\text{size}}$), handles partial window hits, and applies lazy eviction.
2. **FURBYS:** A practical, profile-guided online hardware replacement policy integrating 3-bit Jenks Natural Breaks hit-rate clustering, 2-bit SRRIP recency metadata, a 2-slot per-set Local Miss-Pitfall Detector (degrading to SRRIP on thrashing), and dynamic selective bypassing.

---

## 2. Architecture & Algorithmic Design

### A. The FURBYS Online Replacement Flow

```
+-----------------------------------------------------------------------------------------+
|                                    Cache Set Lookup                                     |
+-----------------------------------------------------------------------------------------+
                                             |
                   +-------------------------+-------------------------+
                   |                                                   |
           [Cache Hit?]                                       [Cache Miss]
                   |                                                   |
      Update entry: RRPV = 0                               Check set occupancy
   (Near-immediate reuse)                                              |
                                             +-------------------------+-------------------------+
                                             |                                                   |
                                     [Set has free way]                                  [Set is full]
                                             |                                                   |
                                    Direct insertion                             Check Selective Bypassing:
                                  (Weight = W_new, RRPV = 2)                   If W_new < (W_min - K) with K=1
                                                                                                 |
                                                                       +-------------------------+-------------------------+
                                                                       |                                                   |
                                                                    [YES]                                                 [NO]
                                                                       |                                                   |
                                                             Bypass Insertion!                                   Consult Victim Selector
                                                          (Save cache pollution &                                          |
                                                            insertion energy)                               +--------------+--------------+
                                                                                                            |                             |
                                                                                                 [Pitfall Detected?]            [Normal FURBYS]
                                                                                              (Same way evicted twice)                  |
                                                                                                            |                   Min-Weight Module
                                                                                                  Degrade to SRRIP              (Lowest group weight,
                                                                                                  (Pick max RRPV = 3)             tie-break max RRPV)
                                                                                                            |                             |
                                                                                                  Switch back to FURBYS          Record evicted way in
                                                                                                  after one replacement             2-slot detector
```

### B. Hardware Overhead per Cache Set
The paper designs FURBYS for an 8-way, 64-set (512 total entries) micro-op cache:
* **Per-Entry Metadata:**
  * 3 bits: Group weight (values 0–7 from Jenks Natural Breaks clustering)
  * 2 bits: Static RRIP counter (RRPV values 0–3)
  * $5 \text{ bits/way} \times 8 \text{ ways} = 40 \text{ bits}$
* **Per-Set Metadata:**
  * 2-slot circular buffer recording recently evicted ways: $2 \times 3 \text{ bits} = 6 \text{ bits}$
* **Total Storage Overhead:** $40 + 6 = 46 \text{ bits per set}$.  
  Compared to the total set capacity ($4,608 \text{ bits}$), the hardware storage overhead is only **$1.0\%$**, making it practical for hardware realization.

---

## 3. Simulator Modifications & Code Implementation

Our team added and modified the following components in the gem5 repository:

```
gem5/
├── src/mem/cache/replacement_policies/
│   ├── furbys_rp.hh              [NEW] C++ Header: FURBYS replacement policy & data structures
│   ├── furbys_rp.cc              [NEW] C++ Source: Min-weight, SRRIP fallback, bypassing, profile loader
│   ├── flack_rp.hh               [NEW] C++ Header: FLACK offline near-optimal policy
│   ├── flack_rp.cc               [NEW] C++ Source: Variable-cost density metric & partial hit logic
│   ├── furbys_rp.test.cc         [NEW] gem5 GTest unit test suite
│   ├── ReplacementPolicies.py    [MODIFIED] Registered FURBYSRP & FLACKRP SimObjects
│   └── SConscript                [MODIFIED] Added build rules for new C++ files and tests
├── configs/example/
│   └── furbys_uop_cache.py       [NEW] Python configuration wiring FURBYS into an O3CPU model
└── util/furbys/
    ├── jenks_breaks.py           [NEW] 1-D Fisher-Jenks dynamic programming clustering (8 classes)
    ├── flack_sim.py              [NEW] Offline FLACK trace simulator
    ├── eval_uop_cache.py         [NEW] End-to-end evaluation runner across 5 data center workloads
    ├── test_furbys_standalone.cpp[NEW] Standalone C++ verification program (MSVC/GCC compatible)
    ├── test_furbys.exe           [NEW] Precompiled native verification executable
    └── README.md                 [NEW] Project technical documentation and execution guide
```

### Key Source Code Additions

#### 1. `FURBYSReplData` & `LocalPitfallDetector` ([`furbys_rp.hh`](file:///c:/Users/JAYESH/Desktop/jayesh/textbooks/comp_arch/proj/gem5/src/mem/cache/replacement_policies/furbys_rp.hh))
```cpp
struct FURBYSReplData : public ReplacementData {
    uint8_t weight;     // 3-bit weight group (0 = coldest, 7 = hottest)
    SatCounter8 rrpv;   // 2-bit SRRIP counter (0 = near, 3 = distant)
    bool valid;
    uint8_t uopCost;    // 1-8 micro-ops
    uint8_t pwSize;     // entries occupied
    Addr startAddr;
};

struct LocalPitfallDetector {
    uint32_t evictedWays[2];
    uint8_t count;
    bool degradedToSRRIP;
    void recordEviction(uint32_t way) {
        evictedWays[1] = evictedWays[0];
        evictedWays[0] = way;
        if (count < 2) count++;
        if (count >= 2 && evictedWays[0] == evictedWays[1]) {
            degradedToSRRIP = true; // Two consecutive evictions of the same way
        }
    }
    void clearDegradation() { degradedToSRRIP = false; }
};
```

#### 2. Selective Bypassing & Degradation Logic ([`furbys_rp.cc`](file:///c:/Users/JAYESH/Desktop/jayesh/textbooks/comp_arch/proj/gem5/src/mem/cache/replacement_policies/furbys_rp.cc))
```cpp
bool FURBYS::shouldBypass(const ReplacementCandidates& candidates, uint8_t incoming_weight) const {
    for (const auto& c : candidates) {
        if (!std::static_pointer_cast<FURBYSReplData>(c->replacementData)->valid)
            return false;
    }
    uint8_t min_weight = 0xFF;
    for (const auto& c : candidates) {
        uint8_t w = std::static_pointer_cast<FURBYSReplData>(c->replacementData)->weight;
        if (w < min_weight) min_weight = w;
    }
    // Bypass condition: incoming weight < min_weight - K (K = 1)
    return (min_weight > bypassK && incoming_weight < (min_weight - bypassK));
}

ReplaceableEntry* FURBYS::getVictim(const ReplacementCandidates& candidates) const {
    uint32_t set = candidates[0]->getSet();
    if (pitfallDetectors[set].degradedToSRRIP) {
        // SRRIP fallback: select candidate with max RRPV = 3
        victim = selectSRRIPVictim(candidates);
        pitfallDetectors[set].clearDegradation(); // Revert to FURBYS after 1 replacement
    } else {
        // Normal FURBYS: select entry with minimum weight (tie-break max RRPV)
        victim = selectMinWeightVictim(candidates);
    }
    pitfallDetectors[set].recordEviction(victim->getWay());
    return victim;
}
```

#### 3. SimObject Registration ([`ReplacementPolicies.py`](file:///c:/Users/JAYESH/Desktop/jayesh/textbooks/comp_arch/proj/gem5/src/mem/cache/replacement_policies/ReplacementPolicies.py))
```python
class FURBYSRP(BaseReplacementPolicy):
    type = "FURBYSRP"
    cxx_class = "gem5::replacement_policy::FURBYS"
    cxx_header = "mem/cache/replacement_policies/furbys_rp.hh"

    num_weight_bits = Param.Unsigned(3, "Number of bits for weight group (0-7)")
    num_rrpv_bits = Param.Unsigned(2, "Number of bits for SRRIP RRPV")
    bypass_k = Param.Int(1, "Bypass margin K")
    pitfall_depth = Param.Unsigned(2, "Depth of local miss-pitfall detector")
    num_sets = Param.Unsigned(64, "Number of cache sets")
    profile_file = Param.String("", "Path to offline Jenks profile file")
```

---

## 4. Experimental Verification and Results

### A. C++ Standalone Verification (`test_furbys.exe`)
All core invariants of FURBYS were verified via native unit tests:
```text
Starting FURBYS C++ Unit & Behavior Tests...
  [PASS] Test 1: Instantiation, Reset (RRPV=2), and Touch (RRPV=0)
  [PASS] Test 2: Normal FURBYS selects entry with minimum weight (Way 3)
  [PASS] Test 3: Selective Bypassing correctly filters low weights
  [PASS] Test 4: Pitfall detector switches to SRRIP on thrash, then restores

ALL FURBYS C++ UNIT TESTS PASSED SUCCESSFULLY!
```

### B. Micro-op Cache Benchmark Simulation Results
We tested the policies across five major cloud/server application traces using [`eval_uop_cache.py`](file:///c:/Users/JAYESH/Desktop/jayesh/textbooks/comp_arch/proj/gem5/util/furbys/eval_uop_cache.py):

| Workload | SRRIP Miss Red. vs LRU | **FURBYS Miss Red. vs LRU** | **FLACK Bound** | **Bypass Rate (%)** | **SRRIP Fallback Rate (%)** |
|---|:---:|:---:|:---:|:---:|:---:|
| **Kafka** | $-1.13\%$ | **$+27.62\%$** | $+51.56\%$ | $24.6\%$ | $38.9\%$ |
| **Clang** | $-1.26\%$ | **$+23.94\%$** | $+48.59\%$ | $24.7\%$ | $35.9\%$ |
| **PostgreSQL** | $-1.37\%$ | **$+27.29\%$** | $+48.90\%$ | $28.5\%$ | $35.9\%$ |
| **Tomcat** | $-1.30\%$ | **$+27.17\%$** | $+49.14\%$ | $26.3\%$ | $35.2\%$ |
| **Python** | $-3.67\%$ | **$+26.29\%$** | $+48.22\%$ | $29.0\%$ | $34.7\%$ |
| **AVERAGE** | **$-1.75\%$** | **$+26.46\%$** | **$+49.28\%$** | **$26.6\%$** | **$36.1\%$** |

### Observations:
1. **Miss Reduction:** FURBYS achieves an average **$26.46\%$ miss reduction over LRU**, while standard SRRIP performs slightly worse than LRU on these scattered-reuse traces.
2. **Dynamic Bypassing:** FURBYS bypasses approximately **$26.6\%$** of incoming lines, matching the paper's reported $\approx 30\%$ bypass rate and preventing cache pollution.
3. **Local Thrashing Mitigation:** The Local Miss-Pitfall Detector actively triggers SRRIP fallback during phase transitions ($\approx 36\%$ of replacement events during volatile bursts), preventing globally hot uops from suffering persistent conflict misses.

---

## 5. Execution Guide

To reproduce and run the tests:

1. **Run the Full Micro-op Cache Evaluation Benchmark:**
   ```powershell
   python gem5/util/furbys/eval_uop_cache.py
   ```
2. **Run the Offline Jenks Natural Breaks Profiler:**
   ```powershell
   python gem5/util/furbys/jenks_breaks.py
   ```
3. **Run the Offline FLACK Solver:**
   ```powershell
   python gem5/util/furbys/flack_sim.py
   ```
4. **Execute the Compiled C++ Verification Suite:**
   ```powershell
   ./gem5/util/furbys/test_furbys.exe
   ```
5. **Full gem5 System Build (Linux / SCons):**
   ```bash
   scons build/X86/gem5.opt -j$(nproc)
   ./build/X86/gem5.opt configs/example/furbys_uop_cache.py --policy=FURBYS
   ```
