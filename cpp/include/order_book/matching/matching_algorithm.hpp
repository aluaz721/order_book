#pragma once

#include "../core/order.hpp"
#include "../core/event.hpp"
#include "../book/price_level_interface.hpp"
#include <vector>
#include <string>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// MatchResult
//
// Returned by MatchingAlgorithm::match_level() describing what happened at
// a single price level. The OrderBook uses this to update order_map_, emit
// FillEvents, and decide whether to continue to the next price level.
// ─────────────────────────────────────────────────────────────────────────────

struct LevelMatchResult {
    uint64_t              quantity_filled;  // total qty filled at this level
    std::vector<FillEvent> fills;           // one per passive order touched
    bool                   level_exhausted; // true if the price level is now empty
};

// ─────────────────────────────────────────────────────────────────────────────
// MatchingAlgorithm
//
// Abstract strategy for allocating fills among resting orders at a single
// price level when an aggressive order arrives. The OrderBook handles
// price-level selection (which prices cross) and the priority queue merge
// (sweeping through levels from best to worst). The MatchingAlgorithm
// handles only intra-level allocation (who gets filled and by how much).
//
// This separation means:
//   - Swapping FIFO for pro-rata only requires changing the MatchingAlgorithm
//   - Both TreeOrderBook and VectorOrderBook work with any MatchingAlgorithm
//   - Benchmarks can isolate matching algorithm latency from book lookup cost
//
// Implementer contract:
//   1. match_level() must not modify the aggressive order's quantity directly.
//      The OrderBook reduces aggressive.quantity by result.quantity_filled.
//   2. match_level() must call level.pop_front() or level.reduce_front()
//      (via PriceLevelInterface) to mutate the passive side.
//   3. FillEvent fields that match_level() must populate:
//        aggressive_order_id, passive_order_id, symbol, aggressor_side,
//        fill_price (= level.price()), fill_quantity, timestamp.
//      The OrderBook fills in `sequence` before emitting via callback.
//   4. match_level() must not throw. Use noexcept where possible.
//   5. match_level() is called once per price level during a sweep. The
//      OrderBook calls it repeatedly from best to worst price until
//      aggressive.quantity == 0 or no more crossing levels exist.
// ─────────────────────────────────────────────────────────────────────────────

class MatchingAlgorithm {
public:
    virtual ~MatchingAlgorithm() = default;

    // Allocate fills between `aggressive` and resting orders at `level`.
    // `remaining_qty` is the aggressive order's remaining unfilled quantity
    // (passed explicitly to avoid requiring the callee to read order.quantity,
    // which may be stale if the OrderBook manages it separately).
    // `timestamp` is the simulation clock at the moment of the match.
    virtual LevelMatchResult match_level(
        const Order&         aggressive,
        uint64_t             remaining_qty,
        PriceLevelInterface& level,
        uint64_t             timestamp) = 0;

    // Human-readable name for logging and benchmark labels.
    virtual const std::string& name() const noexcept = 0;

    // Whether this algorithm ever produces partial fills of individual passive
    // orders. FIFO: yes (front order may be partially filled). Pro-rata: yes
    // (every passive order may receive a fractional allocation).
    // Used by the OrderBook to decide whether to store partially-filled
    // passive orders back into the level or always pop-and-reinsert.
    virtual bool produces_partial_passive_fills() const noexcept = 0;
};

} // namespace order_book