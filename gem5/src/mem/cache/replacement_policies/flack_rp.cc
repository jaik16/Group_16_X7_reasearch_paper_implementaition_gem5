/**
 * Copyright (c) 2025 Antigravity Research / Authors of HPCA 2025 FLACK
 * All rights reserved.
 *
 * Implementation of FLACK: Offline Near-Optimal Micro-op Cache Replacement Policy
 * for Data Center Applications (HPCA 2025).
 */

#include "mem/cache/replacement_policies/flack_rp.hh"

#include <algorithm>
#include <cassert>

#include "base/logging.hh"
#include "sim/cur_tick.hh"

namespace gem5
{

namespace replacement_policy
{

FLACK::FLACK(const Params &p)
    : Base(p),
      uopsPerEntry(p.uops_per_entry),
      numHits(0),
      numMisses(0),
      numPartialHits(0),
      numLazyEvictions(0)
{
}

void
FLACK::invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
{
    auto casted = std::static_pointer_cast<FLACKReplData>(replacement_data);
    casted->valid = false;
}

void
FLACK::touch(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto casted = std::static_pointer_cast<FLACKReplData>(replacement_data);
    casted->lastTouchTick = curTick();
    numHits++;
}

void
FLACK::touch(const std::shared_ptr<ReplacementData>& replacement_data,
             const PacketPtr pkt)
{
    touch(replacement_data);
}

void
FLACK::reset(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto casted = std::static_pointer_cast<FLACKReplData>(replacement_data);
    casted->valid = true;
    casted->lastTouchTick = curTick();
    numMisses++;
}

void
FLACK::reset(const std::shared_ptr<ReplacementData>& replacement_data,
             const PacketPtr pkt)
{
    auto casted = std::static_pointer_cast<FLACKReplData>(replacement_data);
    if (pkt != nullptr) {
        casted->startAddr = pkt->getAddr();
    }
    reset(replacement_data);
}

bool
FLACK::isPartialHit(const ReplacementCandidates& candidates,
                    Addr start_addr, uint32_t req_cost) const
{
    for (const auto& candidate : candidates) {
        auto casted = std::static_pointer_cast<FLACKReplData>(
            candidate->replacementData);
        if (casted->valid && casted->startAddr == start_addr) {
            // Larger PW in cache covers smaller PW request
            if (casted->cost >= req_cost) {
                numPartialHits++;
                return true;
            }
        }
    }
    return false;
}

ReplaceableEntry*
FLACK::getVictim(const ReplacementCandidates& candidates) const
{
    assert(!candidates.empty());

    // 1. Invalid entries first
    for (const auto& candidate : candidates) {
        auto casted = std::static_pointer_cast<FLACKReplData>(
            candidate->replacementData);
        if (!casted->valid) {
            return candidate;
        }
    }

    // 2. Variable-cost and reuse distance evaluation
    // FLACK evicts candidate with maximum (reuse_distance / (cost/size))
    ReplaceableEntry* victim = candidates[0];
    double max_metric = -1.0;

    for (const auto& candidate : candidates) {
        auto casted = std::static_pointer_cast<FLACKReplData>(
            candidate->replacementData);

        Tick reuse = (casted->nextAccessTick > curTick()) ?
            (casted->nextAccessTick - curTick()) : MaxTick;

        // Metric balances reuse distance and micro-op density (cost/size)
        double cost_density = casted->getCostPerEntry();
        double metric = static_cast<double>(reuse) / (cost_density > 0 ? cost_density : 1.0);

        if (metric > max_metric) {
            max_metric = metric;
            victim = candidate;
        }
    }

    return victim;
}

void
FLACK::setFutureTrace(const std::unordered_map<Addr, std::vector<Tick>>& trace)
{
    futureAccessMap = trace;
}

std::shared_ptr<ReplacementData>
FLACK::instantiateEntry()
{
    return std::shared_ptr<ReplacementData>(new FLACKReplData());
}

} // namespace replacement_policy
} // namespace gem5

