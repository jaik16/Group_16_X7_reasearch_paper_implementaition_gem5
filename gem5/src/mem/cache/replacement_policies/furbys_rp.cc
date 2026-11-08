/**
 * Copyright (c) 2025 Antigravity Research / Authors of HPCA 2025 FURBYS
 * All rights reserved.
 *
 * Implementation of FURBYS: Efficient Micro-op Cache Replacement Policy
 * for Data Center Applications (HPCA 2025).
 */

#include "mem/cache/replacement_policies/furbys_rp.hh"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <sstream>

#include "base/logging.hh"

namespace gem5
{

namespace replacement_policy
{

FURBYS::FURBYS(const Params &p)
    : Base(p),
      numWeightBits(p.num_weight_bits),
      numRRPVBits(p.num_rrpv_bits),
      bypassK(p.bypass_k),
      pitfallDepth(p.pitfall_depth),
      numSets(p.num_sets),
      profileFile(p.profile_file),
      numHits(0),
      numMisses(0),
      numBypasses(0),
      numSRRIPVictims(0),
      numFURBYSVictims(0)
{
    fatal_if(numWeightBits == 0, "FURBYS requires at least 1 weight bit.\n");
    fatal_if(numRRPVBits == 0, "FURBYS requires at least 1 RRPV bit.\n");

    pitfallDetectors.resize(numSets > 0 ? numSets : 64);

    if (!profileFile.empty()) {
        loadProfile(profileFile);
    }
}

void
FURBYS::loadProfile(const std::string &filename)
{
    std::ifstream infile(filename);
    if (!infile.is_open()) {
        warn("FURBYS: Unable to open profile file: %s\n", filename.c_str());
        return;
    }

    std::string line;
    while (std::getline(infile, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream iss(line);
        Addr addr;
        unsigned weight;
        if (iss >> std::hex >> addr >> std::dec >> weight) {
            uint8_t max_weight = (1 << numWeightBits) - 1;
            profileTable[addr] = static_cast<uint8_t>(
                std::min(static_cast<unsigned>(max_weight), weight));
        }
    }
}

uint8_t
FURBYS::lookupWeight(Addr addr) const
{
    auto it = profileTable.find(addr);
    if (it != profileTable.end()) {
        return it->second;
    }
    // Default weight if not profiled: median weight tier
    return static_cast<uint8_t>((1 << (numWeightBits - 1)));
}

void
FURBYS::invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
{
    auto casted = std::static_pointer_cast<FURBYSReplData>(replacement_data);
    casted->valid = false;
}

void
FURBYS::touch(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto casted = std::static_pointer_cast<FURBYSReplData>(replacement_data);
    // On hit: reset RRPV to 0 (near-immediate reuse)
    casted->rrpv.reset();
    numHits++;
}

void
FURBYS::touch(const std::shared_ptr<ReplacementData>& replacement_data,
              const PacketPtr pkt)
{
    touch(replacement_data);
}

void
FURBYS::reset(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto casted = std::static_pointer_cast<FURBYSReplData>(replacement_data);
    // Initialize RRPV to 2 (long re-reference interval)
    casted->rrpv.saturate();
    if (casted->rrpv > 0) {
        casted->rrpv--;
    }
    casted->valid = true;
    numMisses++;
}

void
FURBYS::reset(const std::shared_ptr<ReplacementData>& replacement_data,
              const PacketPtr pkt)
{
    auto casted = std::static_pointer_cast<FURBYSReplData>(replacement_data);

    if (pkt != nullptr) {
        casted->startAddr = pkt->getAddr();
        casted->weight = lookupWeight(casted->startAddr);
    }

    reset(replacement_data);
}

void
FURBYS::setEntryWeight(const std::shared_ptr<ReplacementData>& replacement_data,
                       uint8_t weight) const
{
    auto casted = std::static_pointer_cast<FURBYSReplData>(replacement_data);
    uint8_t max_weight = (1 << numWeightBits) - 1;
    casted->weight = std::min(weight, max_weight);
}

bool
FURBYS::shouldBypass(const ReplacementCandidates& candidates,
                     uint8_t incoming_weight) const
{
    assert(!candidates.empty());

    // Do not bypass if there are invalid (empty) entries
    for (const auto& candidate : candidates) {
        auto casted = std::static_pointer_cast<FURBYSReplData>(
            candidate->replacementData);
        if (!casted->valid) {
            return false;
        }
    }

    // Find the minimum weight residing in the set
    uint8_t min_weight = 0xFF;
    for (const auto& candidate : candidates) {
        auto casted = std::static_pointer_cast<FURBYSReplData>(
            candidate->replacementData);
        if (casted->weight < min_weight) {
            min_weight = casted->weight;
        }
    }

    // Bypass condition: incoming weight < min_weight - K
    if (min_weight > bypassK && incoming_weight < (min_weight - bypassK)) {
        numBypasses++;
        return true;
    }

    return false;
}

ReplaceableEntry*
FURBYS::getVictim(const ReplacementCandidates& candidates) const
{
    assert(!candidates.empty());

    // 1. If any candidate is invalid, choose it immediately
    for (const auto& candidate : candidates) {
        auto casted = std::static_pointer_cast<FURBYSReplData>(
            candidate->replacementData);
        if (!casted->valid) {
            return candidate;
        }
    }

    uint32_t set = candidates[0]->getSet();
    if (set >= pitfallDetectors.size()) {
        pitfallDetectors.resize(set + 1);
    }

    ReplaceableEntry* victim = nullptr;

    // 2. Check if local miss-pitfall detector triggered SRRIP degradation
    if (pitfallDetectors[set].degradedToSRRIP) {
        // SRRIP replacement mode (Max Module)
        bool victim_found = false;
        while (!victim_found) {
            for (const auto& candidate : candidates) {
                auto casted = std::static_pointer_cast<FURBYSReplData>(
                    candidate->replacementData);
                if (casted->rrpv.isSaturated()) {
                    victim = candidate;
                    victim_found = true;
                    break;
                }
            }
            if (!victim_found) {
                // Age all candidates by incrementing RRPVs
                for (const auto& candidate : candidates) {
                    auto casted = std::static_pointer_cast<FURBYSReplData>(
                        candidate->replacementData);
                    casted->rrpv++;
                }
            }
        }

        // Switched back to normal FURBYS after one SRRIP replacement
        pitfallDetectors[set].clearDegradation();
        numSRRIPVictims++;
    } else {
        // 3. Normal FURBYS replacement: select entry with minimum weight
        uint8_t min_weight = 0xFF;
        int max_rrpv = -1;

        for (const auto& candidate : candidates) {
            auto casted = std::static_pointer_cast<FURBYSReplData>(
                candidate->replacementData);

            if (casted->weight < min_weight) {
                min_weight = casted->weight;
                max_rrpv = casted->rrpv;
                victim = candidate;
            } else if (casted->weight == min_weight) {
                // Tie-breaker: pick candidate with higher RRPV
                if (static_cast<int>(casted->rrpv) > max_rrpv) {
                    max_rrpv = casted->rrpv;
                    victim = candidate;
                }
            }
        }
        numFURBYSVictims++;
    }

    // Record evicted way in local miss-pitfall detector
    if (victim != nullptr) {
        pitfallDetectors[set].recordEviction(victim->getWay());
    }

    return victim ? victim : candidates[0];
}

std::shared_ptr<ReplacementData>
FURBYS::instantiateEntry()
{
    return std::shared_ptr<ReplacementData>(new FURBYSReplData(numRRPVBits));
}

} // namespace replacement_policy
} // namespace gem5

