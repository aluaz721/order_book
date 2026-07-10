#pragma once

#include "matching_algorithm.hpp"
#include <string>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// FIFOMatcher
//
// Price-time priority (FIFO) matching algorithm. The most common matching
// algorithm used by equities exchanges (NYSE, NASDAQ, LSE).
//
// Allocation rule:
//   When an aggressive order arrives at a price level, resting orders are
//   filled in strict first-in-first-out order — the oldest resting order
//   is filled first, then the next oldest, and so on, until either the
//   aggressive quantity is exhausted or the level is empty.
//
//   A resting order may be partially filled if the aggressive quantity runs
//   out mid-order; the remainder stays at the front of the queue.
//
// Example (5 resting orders at 150.00, aggressive BUY for 250):
//   Resting queue (front → back): [100], [80], [200], [50], [30]
//   Fill sequence:
//     aggressive: 250 remaining → fills [100] fully  → 150 remaining
//     aggressive: 150 remaining → fills [80]  fully  → 70  remaining
//     aggressive: 70  remaining → fills [200] partially: 70 filled,
//                                                        130 stays at front
//   Result: 2 full fills + 1 partial fill; aggressive exhausted.
//
// Properties:
//   - Deterministic: same input always produces same output
//   - Fair to early arrivals: time priority rewards passive order submission
//   - No pro-rata fragmentation: passive orders are never split across
//     multiple aggressive orders simultaneously
// ─────────────────────────────────────────────────────────────────────────────

class FIFOMatcher : public MatchingAlgorithm {
public:
    FIFOMatcher() = default;

    LevelMatchResult match_level(
        const Order&         aggressive,
        uint64_t             remaining_qty,
        PriceLevelInterface& level,
        uint64_t             timestamp) override;

    const std::string& name() const noexcept override;

    // FIFO can partially fill the front passive order
    bool produces_partial_passive_fills() const noexcept override { return true; }

private:
    static const std::string NAME;
};

} // namespace order_book