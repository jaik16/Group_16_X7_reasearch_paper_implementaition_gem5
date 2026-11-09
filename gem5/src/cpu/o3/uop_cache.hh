/*
 * Copyright (c) 2026 Group 16 (Kabir Gupta, Jayesh Vijaivargia, Mahendra)
 * All rights reserved.
 *
 * Micro-op Cache Implementation for gem5 O3CPU
 * Based on the HPCA 2025 paper:
 * "From Optimal to Practical: Efficient Micro-op Cache Replacement Policies
 *  for Data Center Applications" by Kan Zhu et al.
 */

#ifndef __CPU_O3_UOP_CACHE_HH__
#define __CPU_O3_UOP_CACHE_HH__

#include <algorithm>
#include <cassert>
#include <cmath>
#include <deque>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "arch/generic/pcstate.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/static_inst.hh"

namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

class CPU;

/**
 * A cached micro-op and its parent macro-op, along with exact pre-computed
 * PC and next-PC states to guarantee exact instruction stream replay.
 */
struct CachedUop
{
    StaticInstPtr staticInst;
    StaticInstPtr macroop;
    std::unique_ptr<PCStateBase> pc;
    std::unique_ptr<PCStateBase> nextPC;

    CachedUop() : staticInst(nullptr), macroop(nullptr) {}
    CachedUop(StaticInstPtr sInst, StaticInstPtr mInst,
              const PCStateBase *p = nullptr, const PCStateBase *np = nullptr)
        : staticInst(sInst), macroop(mInst),
          pc(p ? p->clone() : nullptr),
          nextPC(np ? np->clone() : nullptr) {}
    CachedUop(const CachedUop &other)
        : staticInst(other.staticInst), macroop(other.macroop),
          pc(other.pc ? other.pc->clone() : nullptr),
          nextPC(other.nextPC ? other.nextPC->clone() : nullptr) {}
    CachedUop& operator=(const CachedUop &other) {
        if (this != &other) {
            staticInst = other.staticInst;
            macroop = other.macroop;
            pc.reset(other.pc ? other.pc->clone() : nullptr);
            nextPC.reset(other.nextPC ? other.nextPC->clone() : nullptr);
        }
        return *this;
    }
    CachedUop(CachedUop &&other) noexcept = default;
    CachedUop& operator=(CachedUop &&other) noexcept = default;
};

/**
 * A Prediction Window (PW).
 * A PW begins at a branch target or cache line boundary and ends at a predicted
 * taken branch or cache line boundary.
 */
struct PredictionWindow
{
    Addr startPC;
    Addr endPC;
    Addr physBlkAddr; // Physical cache line address for strict inclusiveness
    std::vector<CachedUop> uops;
    unsigned cost;    // Number of micro-ops in the PW
    unsigned size;    // Number of entries occupied (ceil(cost / uopsPerEntry))
    uint8_t weight;   // 3-bit weight group (0..7) for FURBYS (0 = coldest)
    uint64_t pwId;
    bool containsRomUops; // True if window contains microcode ROM instructions

    PredictionWindow()
        : startPC(0), endPC(0), physBlkAddr(0), cost(0), size(0),
          weight(0), pwId(0), containsRomUops(false)
    {}
};

/**
 * A single entry in the micro-op cache.
 */
struct UopCacheEntry
{
    bool valid;
    Addr tag;           // PW start virtual PC
    Addr endPC;         // End PC of the PW
    Addr physBlkAddr;   // Physical line address for icache inclusiveness
    std::vector<CachedUop> uops;
    uint64_t pwId;
    unsigned pwCost;    // Micro-op count
    unsigned pwSize;    // Entry count
    uint8_t weight;     // 3-bit weight for FURBYS (0..7)
    uint8_t rrpv;       // 2-bit RRPV (0..3) for SRRIP / FURBYS fallback
    Tick lastTouchTick; // For LRU
    uint64_t nextAccess; // For offline Belady / FLACK

    UopCacheEntry()
        : valid(false), tag(0), endPC(0), physBlkAddr(0), pwId(0),
          pwCost(0), pwSize(0), weight(0), rrpv(2), lastTouchTick(0),
          nextAccess(UINT64_MAX)
    {}
};

/**
 * A cache set in the micro-op cache.
 */
struct UopCacheSet
{
    std::vector<UopCacheEntry> entries;
    // 2-slot local miss-pitfall detector: records recent evicted way indices.
    std::deque<unsigned> pitfallBuffer;
};

/**
 * Pending insertion item to model asynchronous decoder insertion delay.
 */
struct PendingUopInsertion
{
    PredictionWindow pw;
    Tick readyTick;
};

/**
 * Supported Replacement Policies.
 */
enum class UopCacheReplPolicy
{
    LRU,
    SRRIP,
    BELADY,
    FLACK,   // Cost-Aware Greedy (FLACK-inspired) or Offline Trace Replay
    FURBYS
};

/**
 * Accumulation Buffer:
 * Accumulates decoded micro-ops from the legacy decode pipeline into a PW.
 */
class AccumulationBuffer
{
  private:
    PredictionWindow currentPW;
    bool accumulating;
    unsigned maxUopsPerEntry;

  public:
    AccumulationBuffer(unsigned uops_per_entry = 8)
        : accumulating(false), maxUopsPerEntry(uops_per_entry ? uops_per_entry : 8)
    {}

    void reset()
    {
        currentPW = PredictionWindow();
        accumulating = false;
    }

    void startWindow(Addr startPC, Addr physBlkAddr = 0, uint8_t weight = 0)
    {
        currentPW = PredictionWindow();
        currentPW.startPC = startPC;
        currentPW.physBlkAddr = physBlkAddr;
        currentPW.weight = weight;
        accumulating = true;
    }

    void addUop(const StaticInstPtr &staticInst, const StaticInstPtr &macroop,
                const PCStateBase &this_pc, const PCStateBase &next_pc,
                bool inRom, Addr physBlkAddr = 0)
    {
        if (!accumulating) {
            startWindow(this_pc.instAddr(), physBlkAddr);
        }
        currentPW.uops.emplace_back(staticInst, macroop, &this_pc, &next_pc);
        if (inRom) {
            currentPW.containsRomUops = true;
        }
        currentPW.endPC = this_pc.instAddr();
        currentPW.physBlkAddr = physBlkAddr;
        currentPW.cost = currentPW.uops.size();
        currentPW.size = (currentPW.cost + maxUopsPerEntry - 1) / maxUopsPerEntry;
    }

    bool hasPendingPW() const
    {
        return accumulating && !currentPW.uops.empty();
    }

    Addr getStartPC() const
    {
        return currentPW.startPC;
    }

    PredictionWindow finalizeWindow(Addr endPC)
    {
        currentPW.endPC = endPC;
        currentPW.cost = currentPW.uops.size();
        currentPW.size = (currentPW.cost + maxUopsPerEntry - 1) / maxUopsPerEntry;
        PredictionWindow pw = currentPW;
        reset();
        return pw;
    }
};

/**
 * Micro-op Cache for the O3 CPU.
 */
class UopCache
{
  protected:
    CPU *cpu;

    // Configuration parameters
    bool enabled;
    unsigned numEntries;
    unsigned numWays;
    unsigned uopsPerEntry;
    unsigned numSets;
    unsigned setMask;
    unsigned setShift;
    bool isSetPowerOfTwo;
    UopCacheReplPolicy replPolicy;
    Cycles switchDelay;
    Cycles decodeDelay;
    bool inclusive;
    int furbysBypassThresholdK;
    unsigned pitfallDepth;

    // Sets and storage
    std::vector<UopCacheSet> sets;
    std::deque<PendingUopInsertion> pendingInsertions;

    // FURBYS offline profile weight table (Addr -> weight 0..7)
    std::unordered_map<Addr, uint8_t> furbysWeights;

    // Belady / FLACK offline access trace mapping (Addr -> list of future access times)
    std::unordered_map<Addr, std::deque<uint64_t>> futureAccessTrace;
    uint64_t accessCounter;
    uint64_t nextPwId;

    // Trace dump output file for offline profiling
    std::string profileOutputFilePath;
    std::ofstream profileOutputStream;

  public:
    struct UopCacheStatGroup : public statistics::Group
    {
        UopCacheStatGroup(statistics::Group *parent);

        statistics::Scalar lookups;
        statistics::Scalar hits;
        statistics::Scalar partialHits;
        statistics::Scalar misses;
        statistics::Scalar uopHits;
        statistics::Scalar uopMisses;
        statistics::Scalar insertions;
        statistics::Scalar evictions;
        statistics::Scalar bypasses;
        statistics::Scalar furbysNormalDecisions;
        statistics::Scalar furbysDegradedDecisions;
        statistics::Scalar inclusivenessInvalidations;
        statistics::Formula hitRate;
        statistics::Formula uopHitRate;
    } stats;

  public:
    UopCache(CPU *_cpu, const BaseO3CPUParams &params);
    ~UopCache();

    bool isEnabled() const { return enabled; }
    Cycles getSwitchDelay() const { return switchDelay; }
    Cycles getDecodeDelay() const { return decodeDelay; }

    /** Advance asynchronous insertions */
    void tick();

    /**
     * Lookup micro-op cache by starting PC.
     * Returns true on hit (populating outUops), false on miss.
     */
    bool lookup(Addr pc, std::vector<CachedUop> &outUops, bool &partialHit);

    /**
     * Insert a completed Prediction Window into the micro-op cache.
     * Takes into account decode latency (asynchronous insertion).
     */
    void insert(const PredictionWindow &pw);

    /** Invalidate cache entries by physical icache line address (inclusiveness). */
    void invalidateByIcacheAddr(Addr icacheBlkAddr, Addr blkSize);

    /** Flush/clear all entries. */
    void clear();

    /** Get 3-bit FURBYS weight for an address from profiling table (default: 0). */
    uint8_t getWeightForAddress(Addr addr) const;

    /** Load FURBYS weight file generated by offline Jenks clustering. */
    bool loadWeightFile(const std::string &path);

    /** Load future access trace for Belady or FLACK offline policies. */
    bool loadTraceFile(const std::string &path);

    /** Increment statistics from frontend */
    void incUopMisses(unsigned count = 1) { stats.uopMisses += count; }
    void incUopHits(unsigned count = 1) { stats.uopHits += count; }
    void incPartialHits(unsigned count = 1) { stats.partialHits += count; }

  protected:
    unsigned getSetIndex(Addr pc) const;
    void doInsert(const PredictionWindow &pw);
    uint64_t getNextAccess(Addr pc, uint64_t currentAccess);

    // Replacement Policy Helpers
    unsigned selectVictimLRU(unsigned setIdx);
    unsigned peekVictimSRRIP(unsigned setIdx);
    unsigned selectVictimSRRIP(unsigned setIdx);
    unsigned selectVictimBelady(unsigned setIdx);
    unsigned selectVictimFLACK(unsigned setIdx, const PredictionWindow &newPW, bool &bypass);
    unsigned selectVictimFURBYS(unsigned setIdx, const PredictionWindow &newPW, bool &bypass);

    void recordProfileTrace(const char *eventType, Addr pc, unsigned cost, unsigned size);
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_UOP_CACHE_HH__
