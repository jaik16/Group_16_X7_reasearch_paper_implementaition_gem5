/*
 * Copyright (c) 2026 Group 16 (Kabir Gupta, Jayesh Vijaivargia, Mahendra)
 * All rights reserved.
 *
 * Micro-op Cache Implementation for gem5 O3CPU
 * Based on the HPCA 2025 paper:
 * "From Optimal to Practical: Efficient Micro-op Cache Replacement Policies
 *  for Data Center Applications" by Kan Zhu et al.
 */

#include "cpu/o3/uop_cache.hh"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <sstream>

#include "base/trace.hh"
#include "cpu/o3/cpu.hh"
#include "debug/UopCache.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{
namespace o3
{

UopCache::UopCacheStatGroup::UopCacheStatGroup(statistics::Group *parent)
    : statistics::Group(parent, "uop_cache"),
      ADD_STAT(lookups, statistics::units::Count::get(),
               "Total micro-op cache lookups"),
      ADD_STAT(hits, statistics::units::Count::get(),
               "Total micro-op cache hits"),
      ADD_STAT(partialHits, statistics::units::Count::get(),
               "Total micro-op cache partial hits"),
      ADD_STAT(misses, statistics::units::Count::get(),
               "Total micro-op cache misses"),
      ADD_STAT(uopHits, statistics::units::Count::get(),
               "Total micro-ops delivered from micro-op cache"),
      ADD_STAT(uopMisses, statistics::units::Count::get(),
               "Total micro-ops missed in micro-op cache"),
      ADD_STAT(insertions, statistics::units::Count::get(),
               "Total insertions into micro-op cache"),
      ADD_STAT(evictions, statistics::units::Count::get(),
               "Total evictions from micro-op cache"),
      ADD_STAT(bypasses, statistics::units::Count::get(),
               "Total bypassed insertions"),
      ADD_STAT(furbysNormalDecisions, statistics::units::Count::get(),
               "Total FURBYS min-weight victim selections"),
      ADD_STAT(furbysDegradedDecisions, statistics::units::Count::get(),
               "Total FURBYS degraded (SRRIP) victim selections due to pitfall"),
      ADD_STAT(inclusivenessInvalidations, statistics::units::Count::get(),
               "Total micro-op cache entries invalidated by L1 icache inclusiveness"),
      ADD_STAT(hitRate, statistics::units::Ratio::get(),
               "Micro-op cache window hit rate", hits / lookups),
      ADD_STAT(uopHitRate, statistics::units::Ratio::get(),
               "Micro-op cache micro-op hit rate",
               uopHits / (uopHits + uopMisses))
{
    lookups.prereq(lookups);
    hits.prereq(hits);
    partialHits.prereq(partialHits);
    misses.prereq(misses);
    uopHits.prereq(uopHits);
    uopMisses.prereq(uopMisses);
    insertions.prereq(insertions);
    evictions.prereq(evictions);
    bypasses.prereq(bypasses);
    furbysNormalDecisions.prereq(furbysNormalDecisions);
    furbysDegradedDecisions.prereq(furbysDegradedDecisions);
    inclusivenessInvalidations.prereq(inclusivenessInvalidations);
}

UopCache::UopCache(CPU *_cpu, const BaseO3CPUParams &params)
    : cpu(_cpu),
      enabled(params.enableUopCache),
      numEntries(params.uopCacheEntries),
      numWays(params.uopCacheAssoc),
      uopsPerEntry(params.uopCacheUopsPerEntry),
      replPolicy(UopCacheReplPolicy::LRU),
      switchDelay(params.uopCacheSwitchDelay),
      decodeDelay(params.uopCacheDecodeDelay),
      inclusive(params.uopCacheInclusive),
      furbysBypassThresholdK(params.uopCacheFurbysBypassThreshold),
      pitfallDepth(params.uopCachePitfallDepth),
      accessCounter(0),
      nextPwId(1),
      profileOutputFilePath(params.uopCacheProfileOutputFile),
      stats(_cpu)
{
    // Open profile trace output file if specified (needed for size-0 profiling runs)
    if (!profileOutputFilePath.empty()) {
        profileOutputStream.open(profileOutputFilePath, std::ios::out | std::ios::trunc);
    }

    // Support numEntries == 0 to allow paper's size-0 trace-collection run
    // Keeps cache "enabled" so fetch.cc continues recording LOOKUP & INSERT
    if (numEntries == 0) {
        numSets = 0;
        setMask = 0;
        setShift = 2;
        isSetPowerOfTwo = true;
        DPRINTF(UopCache, "UopCache initialized with size 0 (trace collection mode)\n");
        return;
    }

    if (numWays == 0) {
        numWays = 8;
    }

    numSets = numEntries / numWays;
    if (numSets == 0) {
        numSets = 1;
    }

    // Set indexing: support both power-of-two (mask) and non-power-of-two (modulo)
    isSetPowerOfTwo = (numSets & (numSets - 1)) == 0;
    setMask = isSetPowerOfTwo ? (numSets - 1) : 0;
    setShift = 2; // 4-byte instruction alignment

    sets.resize(numSets);
    for (unsigned s = 0; s < numSets; ++s) {
        sets[s].entries.resize(numWays);
    }

    std::string pol = params.uopCacheReplPolicy;
    std::transform(pol.begin(), pol.end(), pol.begin(), ::tolower);
    if (pol == "srrip") {
        replPolicy = UopCacheReplPolicy::SRRIP;
    } else if (pol == "belady") {
        replPolicy = UopCacheReplPolicy::BELADY;
    } else if (pol == "flack") {
        replPolicy = UopCacheReplPolicy::FLACK;
    } else if (pol == "furbys") {
        replPolicy = UopCacheReplPolicy::FURBYS;
    } else {
        replPolicy = UopCacheReplPolicy::LRU;
    }

    if (!params.uopCacheFurbysWeightFile.empty()) {
        loadWeightFile(params.uopCacheFurbysWeightFile);
    }

    if (!params.uopCacheTraceFile.empty()) {
        loadTraceFile(params.uopCacheTraceFile);
    }

    DPRINTF(UopCache, "UopCache initialized: enabled=%d, sets=%d (powerOfTwo=%d), ways=%d, "
                      "uops/entry=%d, repl=%s, inclusive=%d\n",
            enabled, numSets, isSetPowerOfTwo, numWays, uopsPerEntry, pol.c_str(), inclusive);
}

UopCache::~UopCache()
{
    if (profileOutputStream.is_open()) {
        profileOutputStream.close();
    }
}

unsigned
UopCache::getSetIndex(Addr pc) const
{
    if (numSets == 0)
        return 0;

    if (isSetPowerOfTwo) {
        return (pc >> setShift) & setMask;
    } else {
        return (pc >> setShift) % numSets;
    }
}

void
UopCache::recordProfileTrace(const char *eventType, Addr pc, unsigned cost, unsigned size)
{
    if (profileOutputStream.is_open()) {
        profileOutputStream << eventType << " 0x" << std::hex << pc
                            << std::dec << " " << cost << " " << size << "\n";
    }
}

void
UopCache::tick()
{
    if (!enabled)
        return;

    // Process pending asynchronous insertions after decode latency
    while (!pendingInsertions.empty() && pendingInsertions.front().readyTick <= curTick()) {
        PredictionWindow pw = pendingInsertions.front().pw;
        pendingInsertions.pop_front();
        doInsert(pw);
    }
}

uint64_t
UopCache::getNextAccess(Addr pc, uint64_t currentAccess)
{
    auto it = futureAccessTrace.find(pc);
    if (it != futureAccessTrace.end()) {
        auto &q = it->second;
        // Pop past accesses (less than or equal to current access)
        while (!q.empty() && q.front() <= currentAccess) {
            q.pop_front();
        }
        if (!q.empty()) {
            return q.front();
        }
    }
    return UINT64_MAX; // No further access known
}

bool
UopCache::lookup(Addr pc, std::vector<CachedUop> &outUops, bool &partialHit)
{
    if (!enabled)
        return false;

    stats.lookups++;
    accessCounter++;
    recordProfileTrace("LOOKUP", pc, 0, 0);

    // If cache size is 0 (trace collection mode), always miss after logging lookup
    if (numSets == 0) {
        stats.misses++;
        return false;
    }

    unsigned setIdx = getSetIndex(pc);
    auto &set = sets[setIdx];

    partialHit = false;

    for (unsigned w = 0; w < numWays; ++w) {
        auto &entry = set.entries[w];
        if (entry.valid && entry.tag == pc) {
            // Hit in the micro-op cache!
            stats.hits++;
            outUops = entry.uops;

            // Update replacement metadata on hit
            entry.lastTouchTick = curTick();
            if (replPolicy == UopCacheReplPolicy::SRRIP ||
                replPolicy == UopCacheReplPolicy::FURBYS) {
                entry.rrpv = 0; // Hit promotion
            }

            // Belady future access update (accessCounter was incremented above, so current is accessCounter - 1)
            entry.nextAccess = getNextAccess(pc, accessCounter - 1);

            DPRINTF(UopCache, "UopCache HIT at PC 0x%lx (set %u, way %u, uops %zu)\n",
                    pc, setIdx, w, entry.uops.size());
            return true;
        }
    }

    // Miss in micro-op cache
    stats.misses++;
    DPRINTF(UopCache, "UopCache MISS at PC 0x%lx (set %u)\n", pc, setIdx);
    return false;
}

void
UopCache::insert(const PredictionWindow &pw)
{
    if (!enabled || pw.uops.empty())
        return;

    // Do not cache windows containing microcode (ROM) operations
    if (pw.containsRomUops)
        return;

    if (decodeDelay > Cycles(0)) {
        // Model asynchronous insertion delay through decode pipeline
        Tick ready = curTick() + cpu->cyclesToTicks(decodeDelay);
        pendingInsertions.push_back({pw, ready});
    } else {
        doInsert(pw);
    }
}

void
UopCache::doInsert(const PredictionWindow &pw)
{
    // Record insertion in profile output stream if tracing (runs even for size-0 mode)
    recordProfileTrace("INSERT", pw.startPC, pw.cost, pw.size);

    if (numSets == 0)
        return;

    if (pw.containsRomUops)
        return;

    unsigned setIdx = getSetIndex(pw.startPC);
    auto &set = sets[setIdx];

    unsigned neededWays = std::min(numWays, std::max(1u, pw.size));

    // Check if an entry with the same start PC is already present
    // Paper Section II-D: Keep larger prediction windows for partial hits
    for (unsigned w = 0; w < numWays; ++w) {
        auto &entry = set.entries[w];
        if (entry.valid && entry.tag == pw.startPC) {
            if (pw.cost >= entry.pwCost) {
                // Replace with larger window to maximize partial hit opportunity
                entry.endPC = pw.endPC;
                entry.physBlkAddr = pw.physBlkAddr;
                entry.uops = pw.uops;
                entry.pwCost = pw.cost;
                entry.pwSize = neededWays;
                entry.weight = pw.weight;
                entry.rrpv = 2;
                entry.lastTouchTick = curTick();
                entry.nextAccess = getNextAccess(pw.startPC, accessCounter > 0 ? accessCounter - 1 : 0);
                DPRINTF(UopCache, "UopCache updated existing PW with larger window at PC 0x%lx\n",
                        pw.startPC);
            }
            return;
        }
    }

    // 1. Count ways currently occupied by valid PWs in the set
    unsigned waysUsed = 0;
    for (unsigned w = 0; w < numWays; ++w) {
        if (set.entries[w].valid) {
            waysUsed += set.entries[w].pwSize;
        }
    }

    // 2. Bypass is only considered when eviction would be required
    bool bypass = false;
    if (waysUsed + neededWays > numWays) {
        if (replPolicy == UopCacheReplPolicy::FURBYS) {
            uint8_t minWeight = 255;
            bool hasValid = false;
            for (unsigned w = 0; w < numWays; ++w) {
                if (set.entries[w].valid) {
                    minWeight = std::min(minWeight, set.entries[w].weight);
                    hasValid = true;
                }
            }
            if (hasValid && (int)pw.weight + furbysBypassThresholdK < (int)minWeight) {
                bypass = true;
            }
        } else if (replPolicy == UopCacheReplPolicy::FLACK) {
            double newUnitCost = (double)pw.cost / (double)neededWays;
            double minUnitCost = 1e12;
            bool hasValid = false;
            for (unsigned w = 0; w < numWays; ++w) {
                if (set.entries[w].valid) {
                    minUnitCost = std::min(minUnitCost,
                        (double)set.entries[w].pwCost / (double)set.entries[w].pwSize);
                    hasValid = true;
                }
            }
            if (hasValid && newUnitCost < minUnitCost) {
                bypass = true;
            }
        }
    }

    if (bypass) {
        stats.bypasses++;
        DPRINTF(UopCache, "UopCache BYPASS insertion for PC 0x%lx (weight %u)\n",
                pw.startPC, pw.weight);
        return;
    }

    // 3. Evict victim entries until waysUsed + neededWays <= numWays
    while (waysUsed + neededWays > numWays) {
        unsigned victimWay = 0;
        bool dummyBypass = false;

        switch (replPolicy) {
          case UopCacheReplPolicy::LRU:
            victimWay = selectVictimLRU(setIdx);
            break;
          case UopCacheReplPolicy::SRRIP:
            victimWay = selectVictimSRRIP(setIdx);
            break;
          case UopCacheReplPolicy::BELADY:
            victimWay = selectVictimBelady(setIdx);
            break;
          case UopCacheReplPolicy::FLACK:
            victimWay = selectVictimFLACK(setIdx, pw, dummyBypass);
            break;
          case UopCacheReplPolicy::FURBYS:
            victimWay = selectVictimFURBYS(setIdx, pw, dummyBypass);
            break;
          default:
            victimWay = selectVictimLRU(setIdx);
            break;
        }

        auto &victimEntry = set.entries[victimWay];
        if (victimEntry.valid) {
            waysUsed -= victimEntry.pwSize;
            victimEntry.valid = false;
            victimEntry.uops.clear();
            stats.evictions++;
            DPRINTF(UopCache, "UopCache EVICT way %u (PC 0x%lx, weight %u)\n",
                    victimWay, victimEntry.tag, victimEntry.weight);
        } else {
            break; // No more valid entries to evict
        }
    }

    // 4. Allocate the new PW into an available invalid slot
    for (unsigned w = 0; w < numWays; ++w) {
        auto &entry = set.entries[w];
        if (!entry.valid) {
            entry.valid = true;
            entry.tag = pw.startPC;
            entry.endPC = pw.endPC;
            entry.physBlkAddr = pw.physBlkAddr;
            entry.uops = pw.uops;
            entry.pwCost = pw.cost;
            entry.pwSize = neededWays;
            entry.weight = pw.weight;
            entry.rrpv = 2; // Initial RRPV
            entry.lastTouchTick = curTick();
            entry.nextAccess = getNextAccess(pw.startPC, accessCounter > 0 ? accessCounter - 1 : 0);

            stats.insertions++; // Count ONLY real allocations
            DPRINTF(UopCache, "UopCache allocated PW into way %u at PC 0x%lx (uops %zu, size %u, weight %u)\n",
                    w, pw.startPC, pw.uops.size(), entry.pwSize, pw.weight);
            return;
        }
    }
}

unsigned
UopCache::selectVictimLRU(unsigned setIdx)
{
    auto &set = sets[setIdx];
    Tick oldest = MaxTick;
    unsigned victim = 0;

    for (unsigned w = 0; w < numWays; ++w) {
        if (!set.entries[w].valid)
            continue;
        if (set.entries[w].lastTouchTick < oldest) {
            oldest = set.entries[w].lastTouchTick;
            victim = w;
        }
    }
    return victim;
}

unsigned
UopCache::peekVictimSRRIP(unsigned setIdx)
{
    // Finds candidate with max RRPV without modifying state
    auto &set = sets[setIdx];
    uint8_t maxRRPV = 0;
    unsigned candidate = 0;

    for (unsigned w = 0; w < numWays; ++w) {
        if (!set.entries[w].valid)
            continue;
        if (set.entries[w].rrpv >= maxRRPV) {
            maxRRPV = set.entries[w].rrpv;
            candidate = w;
        }
    }
    return candidate;
}

unsigned
UopCache::selectVictimSRRIP(unsigned setIdx)
{
    auto &set = sets[setIdx];

    bool hasValid = false;
    for (unsigned w = 0; w < numWays; ++w) {
        if (set.entries[w].valid) {
            hasValid = true;
            break;
        }
    }
    if (!hasValid)
        return 0;

    // Standard SRRIP: search for RRPV == 3; age entries until one reaches 3
    while (true) {
        for (unsigned w = 0; w < numWays; ++w) {
            if (!set.entries[w].valid)
                continue;
            if (set.entries[w].rrpv >= 3) {
                return w;
            }
        }
        for (unsigned w = 0; w < numWays; ++w) {
            if (set.entries[w].valid && set.entries[w].rrpv < 3) {
                set.entries[w].rrpv++;
            }
        }
    }
}

unsigned
UopCache::selectVictimBelady(unsigned setIdx)
{
    auto &set = sets[setIdx];
    uint64_t furthest = 0;
    unsigned victim = 0;

    for (unsigned w = 0; w < numWays; ++w) {
        if (!set.entries[w].valid)
            continue;
        if (set.entries[w].nextAccess == UINT64_MAX) {
            return w; // No future access, prime victim
        }
        if (set.entries[w].nextAccess > furthest) {
            furthest = set.entries[w].nextAccess;
            victim = w;
        }
    }
    return victim;
}

unsigned
UopCache::selectVictimFLACK(unsigned setIdx, const PredictionWindow &newPW, bool &bypass)
{
    // Cost-Aware Greedy (FLACK-inspired) victim selection
    auto &set = sets[setIdx];
    double minUnitCost = 1e12;
    unsigned minWay = 0;

    for (unsigned w = 0; w < numWays; ++w) {
        if (!set.entries[w].valid) {
            continue;
        }
        auto &entry = set.entries[w];
        double entryUnitCost = (double)entry.pwCost / (double)(entry.pwSize > 0 ? entry.pwSize : 1);
        if (entryUnitCost < minUnitCost) {
            minUnitCost = entryUnitCost;
            minWay = w;
        }
    }

    bypass = false;
    return minWay;
}

unsigned
UopCache::selectVictimFURBYS(unsigned setIdx, const PredictionWindow &newPW, bool &bypass)
{
    auto &set = sets[setIdx];

    // Step 1: Min Module with tie-breaking
    // Tie-break rule: lowest weight -> highest RRPV -> oldest lastTouchTick
    uint8_t minWeight = 255;
    uint8_t maxRRPV = 0;
    Tick oldestTick = MaxTick;
    unsigned furbysVictim = 0;

    for (unsigned w = 0; w < numWays; ++w) {
        if (!set.entries[w].valid) {
            continue;
        }

        auto &entry = set.entries[w];
        bool isBetter = false;

        if (entry.weight < minWeight) {
            isBetter = true;
        } else if (entry.weight == minWeight) {
            if (entry.rrpv > maxRRPV) {
                isBetter = true;
            } else if (entry.rrpv == maxRRPV) {
                if (entry.lastTouchTick < oldestTick) {
                    isBetter = true;
                }
            }
        }

        if (isBetter) {
            minWeight = entry.weight;
            maxRRPV = entry.rrpv;
            oldestTick = entry.lastTouchTick;
            furbysVictim = w;
        }
    }

    bypass = false;

    // Step 2: 2-slot local miss-pitfall detection (Paper Section V & VI-D)
    // Paper: "a two-slot miss-pitfall detector is used for each cache set to store
    // recently evicted ways. When we evict the same way twice, we will switch to SRRIP
    // to evict hot PWs that are temporarily cold. After that, we switch back to normal FURBYS."
    bool isPitfall = false;
    for (unsigned way : set.pitfallBuffer) {
        if (way == furbysVictim) {
            isPitfall = true;
            break;
        }
    }

    unsigned chosenVictim = furbysVictim;
    if (isPitfall) {
        // Repeated eviction detected in 2-slot history: degrade to SRRIP!
        chosenVictim = selectVictimSRRIP(setIdx);
        set.pitfallBuffer.clear(); // Reset detector after fallback
        stats.furbysDegradedDecisions++;
        DPRINTF(UopCache, "UopCache FURBYS pitfall triggered on way %u! Fallback to SRRIP -> way %u\n",
                furbysVictim, chosenVictim);
    } else {
        set.pitfallBuffer.push_back(furbysVictim);
        if (set.pitfallBuffer.size() > pitfallDepth) {
            set.pitfallBuffer.pop_front();
        }
        stats.furbysNormalDecisions++;
    }

    return chosenVictim;
}

void
UopCache::invalidateByIcacheAddr(Addr icacheBlkAddr, Addr blkSize)
{
    if (!enabled || !inclusive || numSets == 0)
        return;

    Addr blkMask = ~Addr(blkSize - 1);
    Addr targetPhysBlk = icacheBlkAddr & blkMask;

    for (unsigned s = 0; s < numSets; ++s) {
        for (unsigned w = 0; w < numWays; ++w) {
            auto &entry = sets[s].entries[w];
            if (entry.valid && (entry.physBlkAddr & blkMask) == targetPhysBlk) {
                entry.valid = false;
                entry.uops.clear();
                stats.inclusivenessInvalidations++;
                DPRINTF(UopCache, "UopCache inclusiveness invalidation: physBlk 0x%lx (set %u, way %u)\n",
                        targetPhysBlk, s, w);
            }
        }
    }
}

void
UopCache::clear()
{
    for (auto &set : sets) {
        for (auto &entry : set.entries) {
            entry.valid = false;
            entry.uops.clear();
        }
        set.pitfallBuffer.clear();
    }
    pendingInsertions.clear();
}

uint8_t
UopCache::getWeightForAddress(Addr addr) const
{
    auto it = furbysWeights.find(addr);
    if (it != furbysWeights.end()) {
        return it->second;
    }
    // Default weight for unprofiled PWs: 0 (coldest group, matching paper)
    return 0;
}

bool
UopCache::loadWeightFile(const std::string &path)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        warn("UopCache: Unable to open FURBYS weight file: %s\n", path);
        return false;
    }

    std::string line;
    unsigned count = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::stringstream ss(line);
        std::string addrStr;
        unsigned weight = 0;
        if (ss >> addrStr >> weight) {
            Addr addr = 0;
            if (addrStr.find("0x") == 0 || addrStr.find("0X") == 0) {
                addr = std::stoull(addrStr, nullptr, 16);
            } else {
                addr = std::stoull(addrStr, nullptr, 10);
            }
            furbysWeights[addr] = (uint8_t)(weight & 0x7);
            count++;
        }
    }
    inform("UopCache: Loaded %u FURBYS weights from %s\n", count, path);
    return true;
}

bool
UopCache::loadTraceFile(const std::string &path)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        warn("UopCache: Unable to open trace file: %s\n", path);
        return false;
    }

    std::string line;
    uint64_t accessIndex = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::stringstream ss(line);
        std::string tag, addrStr;
        if (ss >> tag >> addrStr) {
            // Index ONLY LOOKUP lines to maintain exact 1:1 synchronization with accessCounter
            if (tag == "LOOKUP") {
                Addr addr = 0;
                if (addrStr.find("0x") == 0 || addrStr.find("0X") == 0) {
                    addr = std::stoull(addrStr, nullptr, 16);
                } else {
                    addr = std::stoull(addrStr, nullptr, 10);
                }
                futureAccessTrace[addr].push_back(accessIndex++);
            }
        }
    }
    inform("UopCache: Loaded %lu future access trace events from %s\n", accessIndex, path);
    return true;
}

} // namespace o3
} // namespace gem5
