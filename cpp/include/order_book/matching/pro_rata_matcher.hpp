#pragma once

#include "matching_algorithm.hpp"
#include <string>
#include <cstdint>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// ProRataMatcher (post-MVP)
//
// Pro-rata (proportional) matching algorithm. Used by some futures and
// options exchanges (CME Eurodollar futures, Euronext equity options).
//
// Allocation rule:
//   When an aggressive order arrives, each resting order at the price level
//   receives a fill proportional to its quantity relative to the total
//   resting quantity at that level.
//
//   allocation_i = floor(aggressive_qty × resting_qty_i / total_resting_qty)
//
//   Because floor() discards fractional lots, the initial allocations will
//   typically sum to less than aggressive_qty. The remainder is distributed
//   to resting orders in descending order of their fractional remainder
//   (i.e. the orders that were most "shorted" by rounding get priority).
//   If remainders are equal, time priority breaks the tie.
//
// Example (3 resting orders at 150.00, aggressive BUY for 100):
//   Resting: order A=200, order B=150, order C=50  (total=400)
//   Proportional allocations:
//     A: floor(100 × 200/400) = 50
//     B: floor(100 × 150/400) = 37
//     C: floor(100 × 50/400)  = 12
//     Sum = 99, remainder = 1
//   Remainder distribution (fractional parts: A=0.0, B=0.5, C=0.5):
//     B and C tie on fractional remainder → time priority → B gets +1
//   Final allocations: A=50, B=38, C=12
//
// Properties:
//   - All resting orders at a level receive partial fills simultaneously
//   - Incentivises posting large passive orders (size ∝ allocation)
//   - More complex than FIFO; higher constant factor in match_level()
//   - Produces more FillEvents per match (one per resting order touched)
//
// Configuration:
//   min_fill_qty  — resting orders smaller than this receive no allocation
//                   in the initial proportional pass (but may receive
//                   remainder fills). Default: 0 (disabled).
//   time_priority_fraction — if > 0, a portion of the aggressive quantity
//                   equal to this fraction is allocated FIFO before the
//                   pro-rata pass begins. Used by "FIFO + pro-rata" hybrid
//                   exchanges (e.g. CME Treasury futures). Default: 0.
//
// NOTE: Post-MVP. Gate with cmake option AQUILA_BUILD_PRO_RATA (default OFF).
// ─────────────────────────────────────────────────────────────────────────────

struct ProRataConfig {
    uint64_t min_fill_qty            = 0;     // 0 = disabled
    double   time_priority_fraction  = 0.0;   // 0.0 = pure pro-rata
};

class ProRataMatcher : public MatchingAlgorithm {
public:
    using Config = ProRataConfig;

    explicit ProRataMatcher(Config config = Config{});

    LevelMatchResult match_level(
        const Order&         aggressive,
        uint64_t             remaining_qty,
        PriceLevelInterface& level,
        uint64_t             timestamp) override;

    const std::string& name() const noexcept override;

    // Pro-rata always produces partial fills of passive orders
    bool produces_partial_passive_fills() const noexcept override { return true; }

    const Config& config() const noexcept { return config_; }

private:
    // Phase 1: FIFO pass for the time-priority fraction (if configured)
    LevelMatchResult fifo_phase(
        const Order&         aggressive,
        uint64_t             fifo_qty,
        PriceLevelInterface& level,
        uint64_t             timestamp);

    // Phase 2: proportional allocation across all remaining resting orders
    LevelMatchResult pro_rata_phase(
        const Order&         aggressive,
        uint64_t             remaining_qty,
        PriceLevelInterface& level,
        uint64_t             timestamp);

    Config            config_;
    static const std::string NAME;
};

} // namespace order_book