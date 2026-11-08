/**
 * Copyright (c) 2025 Antigravity Research / Authors of HPCA 2025 FURBYS
 * All rights reserved.
 *
 * Unit tests for FURBYS replacement policy.
 */

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "mem/cache/replacement_policies/furbys_rp.hh"
#include "params/FURBYSRP.hh"

using namespace gem5;

class FURBYSRPTestF : public ::testing::Test
{
  public:
    std::shared_ptr<replacement_policy::FURBYS> rp;

    FURBYSRPTestF()
    {
        FURBYSRPParams params;
        params.eventq_index = 0;
        params.num_weight_bits = 3;
        params.num_rrpv_bits = 2;
        params.bypass_k = 1;
        params.pitfall_depth = 2;
        params.num_sets = 64;
        params.profile_file = "";
        rp = std::make_shared<replacement_policy::FURBYS>(params);
    }
};

TEST_F(FURBYSRPTestF, InstantiatedEntry)
{
    const auto repl_data = rp->instantiateEntry();
    ASSERT_NE(repl_data, nullptr);

    auto casted = std::static_pointer_cast<
        replacement_policy::FURBYS::FURBYSReplData>(repl_data);
    ASSERT_FALSE(casted->valid);
    ASSERT_EQ(casted->weight, 0);
}

TEST_F(FURBYSRPTestF, WeightAndTouchBehavior)
{
    auto repl_data = rp->instantiateEntry();
    auto casted = std::static_pointer_cast<
        replacement_policy::FURBYS::FURBYSReplData>(repl_data);

    // Initial reset: RRPV initialized to 2 (long re-reference interval)
    rp->reset(repl_data);
    ASSERT_TRUE(casted->valid);
    ASSERT_EQ(casted->rrpv, 2);

    // Touch: hit resets RRPV to 0 (near-immediate reuse)
    rp->touch(repl_data);
    ASSERT_EQ(casted->rrpv, 0);
}

TEST_F(FURBYSRPTestF, MinWeightVictimSelection)
{
    // 4-way set
    std::vector<ReplaceableEntry> entries(4);
    ReplacementCandidates candidates;

    uint8_t test_weights[4] = {5, 2, 7, 1}; // Way 3 has minimum weight 1

    for (int i = 0; i < 4; i++) {
        entries[i].setPosition(0, i);
        entries[i].replacementData = rp->instantiateEntry();
        rp->reset(entries[i].replacementData);
        rp->setEntryWeight(entries[i].replacementData, test_weights[i]);
        candidates.push_back(&entries[i]);
    }

    // Normal FURBYS selects candidate with minimum weight (Way 3, weight 1)
    ReplaceableEntry* victim = rp->getVictim(candidates);
    ASSERT_NE(victim, nullptr);
    ASSERT_EQ(victim->getWay(), 3);
}

TEST_F(FURBYSRPTestF, SelectiveBypassing)
{
    std::vector<ReplaceableEntry> entries(4);
    ReplacementCandidates candidates;

    // Minimum weight in set is 4
    uint8_t test_weights[4] = {4, 6, 5, 7};

    for (int i = 0; i < 4; i++) {
        entries[i].setPosition(0, i);
        entries[i].replacementData = rp->instantiateEntry();
        rp->reset(entries[i].replacementData);
        rp->setEntryWeight(entries[i].replacementData, test_weights[i]);
        candidates.push_back(&entries[i]);
    }

    // Min weight in set is 4. With K = 1, threshold is min_weight - K = 3.
    // Incoming weight 1 < 3: Should bypass!
    ASSERT_TRUE(rp->shouldBypass(candidates, 1));

    // Incoming weight 2 < 3: Should bypass!
    ASSERT_TRUE(rp->shouldBypass(candidates, 2));

    // Incoming weight 3 is not < 3: Should NOT bypass!
    ASSERT_FALSE(rp->shouldBypass(candidates, 3));

    // Incoming weight 5 is >= 3: Should NOT bypass!
    ASSERT_FALSE(rp->shouldBypass(candidates, 5));
}

TEST_F(FURBYSRPTestF, LocalPitfallDetectorDegradation)
{
    std::vector<ReplaceableEntry> entries(4);
    ReplacementCandidates candidates;

    // Set weights and RRPVs
    // Way 1 has weight 1 (lowest weight)
    // Way 2 has weight 6 but RRPV = 3 (distant reuse)
    for (int i = 0; i < 4; i++) {
        entries[i].setPosition(1, i);
        entries[i].replacementData = rp->instantiateEntry();
        rp->reset(entries[i].replacementData);
        candidates.push_back(&entries[i]);
    }

    rp->setEntryWeight(entries[0].replacementData, 5);
    rp->setEntryWeight(entries[1].replacementData, 1); // Minimum weight
    rp->setEntryWeight(entries[2].replacementData, 6);
    rp->setEntryWeight(entries[3].replacementData, 4);

    // First eviction: selects Way 1 (min weight)
    ReplaceableEntry* v1 = rp->getVictim(candidates);
    ASSERT_EQ(v1->getWay(), 1);

    // Second eviction: selects Way 1 again (min weight)
    ReplaceableEntry* v2 = rp->getVictim(candidates);
    ASSERT_EQ(v2->getWay(), 1);

    // Now Way 1 was evicted twice consecutively!
    // The local miss-pitfall detector detects repeated eviction and triggers SRRIP degradation.
    // In SRRIP mode, it will pick the candidate with maximum RRPV rather than min weight!
    auto d0 = std::static_pointer_cast<replacement_policy::FURBYS::FURBYSReplData>(
        entries[0].replacementData);
    auto d2 = std::static_pointer_cast<replacement_policy::FURBYS::FURBYSReplData>(
        entries[2].replacementData);
    d0->rrpv.reset(); // RRPV = 0
    d2->rrpv.saturate(); // RRPV = 3 (Distant)

    // Third eviction should degrade to SRRIP and pick Way 2 (RRPV = 3)
    ReplaceableEntry* v3 = rp->getVictim(candidates);
    ASSERT_EQ(v3->getWay(), 2);

    // After one SRRIP replacement, FURBYS recovers and returns to normal operation!
    ReplaceableEntry* v4 = rp->getVictim(candidates);
    ASSERT_EQ(v4->getWay(), 1); // Back to min weight
}

