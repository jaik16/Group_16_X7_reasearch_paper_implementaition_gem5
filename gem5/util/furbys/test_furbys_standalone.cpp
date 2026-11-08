/**
 * Standalone C++ verification test for FURBYS replacement policy logic.
 * HPCA 2025: Efficient Micro-op Cache Replacement Policies for Data Center Applications.
 */

#include <iostream>
#include <vector>
#include <memory>
#include <cassert>
#include <cstdint>
#include <algorithm>

namespace test_furbys
{

struct SatCounter8
{
    uint8_t val;
    uint8_t max_val;
    SatCounter8(int bits) : val(0), max_val((1 << bits) - 1) {}
    void saturate() { val = max_val; }
    void reset() { val = 0; }
    bool isSaturated() const { return val >= max_val; }
    operator int() const { return val; }
    SatCounter8& operator++(int) { if (val < max_val) val++; return *this; }
    SatCounter8& operator--(int) { if (val > 0) val--; return *this; }
    bool operator>(int o) const { return val > o; }
    bool operator>=(int o) const { return val >= o; }
};

struct FURBYSReplData
{
    uint8_t weight;
    SatCounter8 rrpv;
    bool valid;
    uint32_t cost;
    uint32_t size;
    uint64_t startAddr;

    FURBYSReplData(int rrpv_bits = 2)
        : weight(0), rrpv(rrpv_bits), valid(false), cost(1), size(1), startAddr(0)
    {}
};

struct ReplaceableEntry
{
    uint32_t set;
    uint32_t way;
    std::shared_ptr<FURBYSReplData> replacementData;
    ReplaceableEntry(uint32_t s = 0, uint32_t w = 0) : set(s), way(w) {}
    uint32_t getSet() const { return set; }
    uint32_t getWay() const { return way; }
};

struct LocalPitfallDetector
{
    uint32_t evictedWays[2];
    uint8_t count;
    bool degradedToSRRIP;

    LocalPitfallDetector() : count(0), degradedToSRRIP(false)
    {
        evictedWays[0] = 0xFFFFFFFF;
        evictedWays[1] = 0xFFFFFFFF;
    }

    void recordEviction(uint32_t way)
    {
        evictedWays[1] = evictedWays[0];
        evictedWays[0] = way;
        if (count < 2) count++;
        if (count >= 2 && evictedWays[0] == evictedWays[1]) {
            degradedToSRRIP = true;
        }
    }

    void clearDegradation()
    {
        degradedToSRRIP = false;
    }
};

class FURBYSPolicy
{
public:
    int numWeightBits;
    int numRRPVBits;
    int bypassK;
    int pitfallDepth;
    std::vector<LocalPitfallDetector> pitfallDetectors;

    FURBYSPolicy(int w_bits = 3, int r_bits = 2, int k = 1, int depth = 2, int num_sets = 64)
        : numWeightBits(w_bits), numRRPVBits(r_bits), bypassK(k), pitfallDepth(depth),
          pitfallDetectors(num_sets)
    {}

    std::shared_ptr<FURBYSReplData> instantiateEntry()
    {
        return std::make_shared<FURBYSReplData>(numRRPVBits);
    }

    void touch(std::shared_ptr<FURBYSReplData> data)
    {
        data->rrpv.reset(); // On hit: reset RRPV to 0
    }

    void reset(std::shared_ptr<FURBYSReplData> data, uint8_t w = 4)
    {
        data->weight = w;
        data->rrpv.saturate();
        if (data->rrpv > 0) data->rrpv--; // RRPV = 2
        data->valid = true;
    }

    bool shouldBypass(const std::vector<ReplaceableEntry*>& candidates, uint8_t in_weight)
    {
        for (auto c : candidates) {
            if (!c->replacementData->valid) return false;
        }
        uint8_t min_w = 0xFF;
        for (auto c : candidates) {
            if (c->replacementData->weight < min_w) {
                min_w = c->replacementData->weight;
            }
        }
        return (min_w > bypassK && in_weight < (min_w - bypassK));
    }

    ReplaceableEntry* getVictim(const std::vector<ReplaceableEntry*>& candidates)
    {
        for (auto c : candidates) {
            if (!c->replacementData->valid) return c;
        }

        uint32_t set = candidates[0]->getSet();
        ReplaceableEntry* victim = nullptr;

        if (pitfallDetectors[set].degradedToSRRIP) {
            // SRRIP mode: max RRPV
            bool found = false;
            while (!found) {
                for (auto c : candidates) {
                    if (c->replacementData->rrpv.isSaturated()) {
                        victim = c;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    for (auto c : candidates) {
                        c->replacementData->rrpv++;
                    }
                }
            }
            pitfallDetectors[set].clearDegradation();
        } else {
            // Normal FURBYS: min weight, tie-break max RRPV
            uint8_t min_w = 0xFF;
            int max_r = -1;
            for (auto c : candidates) {
                auto d = c->replacementData;
                if (d->weight < min_w) {
                    min_w = d->weight;
                    max_r = d->rrpv;
                    victim = c;
                } else if (d->weight == min_w && d->rrpv > max_r) {
                    max_r = d->rrpv;
                    victim = c;
                }
            }
        }

        if (victim) {
            pitfallDetectors[set].recordEviction(victim->getWay());
        }
        return victim;
    }
};

} // namespace test_furbys

int main()
{
    using namespace test_furbys;
    std::cout << "Starting FURBYS C++ Unit & Behavior Tests..." << std::endl;

    FURBYSPolicy furbys(3, 2, 1, 2, 64);

    // Test 1: Entry instantiation and touch/reset
    {
        auto data = furbys.instantiateEntry();
        assert(!data->valid);
        furbys.reset(data, 5);
        assert(data->valid);
        assert(data->weight == 5);
        assert(data->rrpv == 2); // Initialized to 2

        furbys.touch(data);
        assert(data->rrpv == 0); // Reset to 0 on hit
        std::cout << "  [PASS] Test 1: Instantiation, Reset (RRPV=2), and Touch (RRPV=0)" << std::endl;
    }

    // Test 2: Min-weight victim selection
    {
        std::vector<ReplaceableEntry> entries(4);
        std::vector<ReplaceableEntry*> candidates;
        uint8_t test_weights[4] = {6, 3, 7, 1}; // Way 3 has minimum weight 1

        for (int i = 0; i < 4; i++) {
            entries[i] = ReplaceableEntry(0, i);
            entries[i].replacementData = furbys.instantiateEntry();
            furbys.reset(entries[i].replacementData, test_weights[i]);
            candidates.push_back(&entries[i]);
        }

        auto victim = furbys.getVictim(candidates);
        assert(victim != nullptr);
        assert(victim->getWay() == 3);
        std::cout << "  [PASS] Test 2: Normal FURBYS selects entry with minimum weight (Way 3)" << std::endl;
    }

    // Test 3: Selective Bypassing
    {
        std::vector<ReplaceableEntry> entries(4);
        std::vector<ReplaceableEntry*> candidates;
        uint8_t test_weights[4] = {5, 6, 4, 7}; // Min weight is 4

        for (int i = 0; i < 4; i++) {
            entries[i] = ReplaceableEntry(0, i);
            entries[i].replacementData = furbys.instantiateEntry();
            furbys.reset(entries[i].replacementData, test_weights[i]);
            candidates.push_back(&entries[i]);
        }

        // Min weight is 4. Threshold is min_w - K = 4 - 1 = 3.
        assert(furbys.shouldBypass(candidates, 1) == true);   // 1 < 3 -> Bypass!
        assert(furbys.shouldBypass(candidates, 2) == true);   // 2 < 3 -> Bypass!
        assert(furbys.shouldBypass(candidates, 3) == false);  // 3 not < 3 -> Don't bypass
        assert(furbys.shouldBypass(candidates, 6) == false);  // 6 >= 3 -> Don't bypass
        std::cout << "  [PASS] Test 3: Selective Bypassing correctly filters low weights" << std::endl;
    }

    // Test 4: Local Miss-Pitfall Detector & SRRIP Degradation
    {
        std::vector<ReplaceableEntry> entries(4);
        std::vector<ReplaceableEntry*> candidates;

        for (int i = 0; i < 4; i++) {
            entries[i] = ReplaceableEntry(1, i);
            entries[i].replacementData = furbys.instantiateEntry();
            furbys.reset(entries[i].replacementData, (i == 1 ? 1 : 6));
            candidates.push_back(&entries[i]);
        }

        // Eviction 1: picks Way 1 (min weight)
        auto v1 = furbys.getVictim(candidates);
        assert(v1->getWay() == 1);

        // Eviction 2: picks Way 1 again (consecutive eviction)
        auto v2 = furbys.getVictim(candidates);
        assert(v2->getWay() == 1);

        // Repeated eviction triggered! Set 1 should be degraded to SRRIP.
        assert(furbys.pitfallDetectors[1].degradedToSRRIP == true);

        // Set RRPVs: Way 2 has max RRPV (3)
        entries[0].replacementData->rrpv.reset(); // 0
        entries[1].replacementData->rrpv.reset(); // 0
        entries[2].replacementData->rrpv.saturate(); // 3
        entries[3].replacementData->rrpv.reset(); // 0

        // Eviction 3: Must degrade to SRRIP and select Way 2 (max RRPV = 3)
        auto v3 = furbys.getVictim(candidates);
        assert(v3->getWay() == 2);

        // After one SRRIP eviction, FURBYS recovers to normal mode!
        assert(furbys.pitfallDetectors[1].degradedToSRRIP == false);
        auto v4 = furbys.getVictim(candidates);
        assert(v4->getWay() == 1); // Back to min weight
        std::cout << "  [PASS] Test 4: Pitfall detector switches to SRRIP on thrash, then restores" << std::endl;
    }

    std::cout << "\nALL FURBYS C++ UNIT TESTS PASSED SUCCESSFULLY!" << std::endl;
    return 0;
}
