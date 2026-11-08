/**
 * Copyright (c) 2025 Antigravity Research / Authors of HPCA 2025 FURBYS
 * All rights reserved.
 *
 * Implementation of FURBYS: Efficient Micro-op Cache Replacement Policy
 * for Data Center Applications (HPCA 2025).
 */

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_FURBYS_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_FURBYS_RP_HH__

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "mem/packet.hh"
#include "params/FURBYSRP.hh"

namespace gem5
{

namespace replacement_policy
{

/**
 * FURBYS (FLACK-based groUping-by-hit-Rate BYpassing-coldness detecting-miSses)
 * replacement policy for micro-op caches.
 *
 * Considers:
 * 1. Offline whole-execution hit-rate grouping via Jenks Natural Breaks (3 bits: 0-7).
 * 2. Local miss-pitfall detection per cache set (2-slot history of evicted ways)
 *    to detect phase changes / locally cold entries and temporarily degrade to SRRIP.
 * 3. Selective bypassing of incoming entries when weight < min_weight - K (K = 1).
 * 4. 2-bit SRRIP metadata per entry for fine-grained recency and degradation fallback.
 */
class FURBYS : public Base
{
  public:
    /** FURBYS replacement data per cache block / entry */
    struct FURBYSReplData : public ReplacementData
    {
        /** 3-bit weight group (0 = coldest, 7 = hottest) */
        uint8_t weight;

        /** 2-bit SRRIP Re-reference Prediction Value (0 = near, 3 = distant) */
        SatCounter8 rrpv;

        /** Whether entry is valid */
        bool valid;

        /** Number of micro-ops stored in this entry (1-8) */
        uint8_t uopCost;

        /** Size of the prediction window in entries */
        uint8_t pwSize;

        /** Start address of the prediction window */
        Addr startAddr;

        FURBYSReplData(int num_rrpv_bits)
            : weight(0), rrpv(num_rrpv_bits), valid(false),
              uopCost(1), pwSize(1), startAddr(0)
        {
        }
    };

    /** Local miss-pitfall detector per cache set (depth = 2) */
    struct LocalPitfallDetector
    {
        uint32_t evictedWays[2];
        uint8_t count;
        bool degradedToSRRIP;

        LocalPitfallDetector()
            : count(0), degradedToSRRIP(false)
        {
            evictedWays[0] = 0xFFFFFFFF;
            evictedWays[1] = 0xFFFFFFFF;
        }

        void recordEviction(uint32_t way)
        {
            evictedWays[1] = evictedWays[0];
            evictedWays[0] = way;
            if (count < 2) {
                count++;
            }
            // If the same way was evicted twice consecutively, trigger SRRIP degradation
            if (count >= 2 && evictedWays[0] == evictedWays[1]) {
                degradedToSRRIP = true;
            }
        }

        void clearDegradation()
        {
            degradedToSRRIP = false;
        }
    };

  protected:
    /** Configuration parameters */
    const unsigned numWeightBits;
    const unsigned numRRPVBits;
    const int bypassK;
    const unsigned pitfallDepth;
    const unsigned numSets;
    const std::string profileFile;

    /** Per-set local miss-pitfall detectors */
    mutable std::vector<LocalPitfallDetector> pitfallDetectors;

    /** Profiled weight table loaded from Jenks Natural Breaks analysis: Addr -> Weight (0-7) */
    std::unordered_map<Addr, uint8_t> profileTable;

    /** Statistics counters */
    mutable uint64_t numHits;
    mutable uint64_t numMisses;
    mutable uint64_t numBypasses;
    mutable uint64_t numSRRIPVictims;
    mutable uint64_t numFURBYSVictims;

    /** Helper to load profile data */
    void loadProfile(const std::string &filename);

    /** Lookup weight for an address */
    uint8_t lookupWeight(Addr addr) const;

  public:
    typedef FURBYSRPParams Params;
    FURBYS(const Params &p);
    ~FURBYS() = default;

    /** Invalidate entry */
    void invalidate(const std::shared_ptr<ReplacementData>& replacement_data) override;

    /** Touch entry on hit */
    void touch(const std::shared_ptr<ReplacementData>& replacement_data) const override;
    void touch(const std::shared_ptr<ReplacementData>& replacement_data,
               const PacketPtr pkt) override;

    /** Reset entry on insertion */
    void reset(const std::shared_ptr<ReplacementData>& replacement_data) const override;
    void reset(const std::shared_ptr<ReplacementData>& replacement_data,
               const PacketPtr pkt) override;

    /** Find victim among replacement candidates */
    ReplaceableEntry* getVictim(const ReplacementCandidates& candidates) const override;

    /** Check if incoming insertion should be selectively bypassed */
    bool shouldBypass(const ReplacementCandidates& candidates, uint8_t incoming_weight) const;

    /** Explicitly set group weight for a PW / entry */
    void setEntryWeight(const std::shared_ptr<ReplacementData>& replacement_data,
                        uint8_t weight) const;

    /** Register replacement data for an entry */
    std::shared_ptr<ReplacementData> instantiateEntry() override;

    /** Statistics accessors */
    uint64_t getHits() const { return numHits; }
    uint64_t getMisses() const { return numMisses; }
    uint64_t getBypasses() const { return numBypasses; }
    uint64_t getSRRIPVictims() const { return numSRRIPVictims; }
    uint64_t getFURBYSVictims() const { return numFURBYSVictims; }
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_FURBYS_RP_HH__

