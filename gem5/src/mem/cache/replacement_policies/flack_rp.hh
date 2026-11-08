/**
 * Copyright (c) 2025 Antigravity Research / Authors of HPCA 2025 FLACK
 * All rights reserved.
 *
 * Implementation of FLACK: Offline Near-Optimal Micro-op Cache Replacement Policy
 * for Data Center Applications (HPCA 2025).
 */

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_FLACK_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_FLACK_RP_HH__

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "base/types.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "mem/packet.hh"
#include "params/FLACKRP.hh"

namespace gem5
{

namespace replacement_policy
{

/**
 * FLACK (FOO-based seLectively-bypassing Asynchronizing Cost-varying selective-data-Keeping)
 * offline near-optimal micro-op cache replacement policy.
 */
class FLACK : public Base
{
  public:
    struct FLACKReplData : public ReplacementData
    {
        /** Variable cost: number of micro-ops (1-32) */
        uint32_t cost;

        /** Size: number of cache entries occupied (1-4) */
        uint32_t size;

        /** Start address of prediction window */
        Addr startAddr;

        /** End address / length of prediction window */
        Addr endAddr;

        /** Next access cycle or reuse distance (for offline optimal calculation) */
        Tick nextAccessTick;

        /** Last touch cycle */
        Tick lastTouchTick;

        /** Whether entry is valid */
        bool valid;

        FLACKReplData()
            : cost(1), size(1), startAddr(0), endAddr(0),
              nextAccessTick(MaxTick), lastTouchTick(0), valid(false)
        {
        }

        /** Cost-effectiveness ratio: micro-ops per entry */
        double getCostPerEntry() const
        {
            return size > 0 ? static_cast<double>(cost) / size : 1.0;
        }
    };

  protected:
    const unsigned uopsPerEntry;

    /** Offline access trace lookup table: Addr -> list of future access ticks */
    mutable std::unordered_map<Addr, std::vector<Tick>> futureAccessMap;

    /** Statistics */
    mutable uint64_t numHits;
    mutable uint64_t numMisses;
    mutable uint64_t numPartialHits;
    mutable uint64_t numLazyEvictions;

  public:
    typedef FLACKRPParams Params;
    FLACK(const Params &p);
    ~FLACK() = default;

    void invalidate(const std::shared_ptr<ReplacementData>& replacement_data) override;
    void touch(const std::shared_ptr<ReplacementData>& replacement_data) const override;
    void touch(const std::shared_ptr<ReplacementData>& replacement_data,
               const PacketPtr pkt) override;

    void reset(const std::shared_ptr<ReplacementData>& replacement_data) const override;
    void reset(const std::shared_ptr<ReplacementData>& replacement_data,
               const PacketPtr pkt) override;

    ReplaceableEntry* getVictim(const ReplacementCandidates& candidates) const override;
    std::shared_ptr<ReplacementData> instantiateEntry() override;

    /** Check if an access is a partial hit in a larger cached PW */
    bool isPartialHit(const ReplacementCandidates& candidates,
                      Addr start_addr, uint32_t req_cost) const;

    /** Register future trace for offline optimal decision making */
    void setFutureTrace(const std::unordered_map<Addr, std::vector<Tick>>& trace);

    uint64_t getHits() const { return numHits; }
    uint64_t getMisses() const { return numMisses; }
    uint64_t getPartialHits() const { return numPartialHits; }
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_FLACK_RP_HH__

