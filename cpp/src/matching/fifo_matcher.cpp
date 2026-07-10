// FIFO Matching Algorithm Implementation

#include "../../include/order_book/matching/fifo_matcher.hpp"
#include <algorithm>

namespace order_book {

// set the static name for the FIFO matcher
const std::string FIFOMatcher::NAME = "FIFO";

// Match the aggressive order against the resting orders at a single price level.
// The FIFO rule always consumes the oldest resting order first and continues
// until either the aggressive quantity is exhausted or the level becomes empty.
LevelMatchResult FIFOMatcher::match_level(
    const Order& aggressive,
    uint64_t remaining_qty,
    PriceLevelInterface& level,
    uint64_t timestamp)
{
    // Initialize the match summary with no fills and no quantity consumed yet.
    LevelMatchResult result{};
    result.quantity_filled = 0;
    result.level_exhausted = false;

    // If there is nothing to do, report whether the level is already empty.
    if (remaining_qty == 0 || level.empty()) {
        result.level_exhausted = level.empty();
        return result;
    }

    // Continue matching while the aggressive order still has quantity left and
    // the price level still has resting liquidity to consume.
    uint64_t qty_remaining = remaining_qty;
    while (qty_remaining > 0 && !level.empty()) {
        const Order& front = level.front();

        // Guard against an empty or already-consumed front order before matching.
        if (front.quantity_remaining == 0) {
            (void)level.pop_front();
            continue;
        }

        // Fill up to the smaller of the remaining aggressive quantity and the
        // front resting order's remaining quantity.
        const uint64_t fill_qty = std::min(qty_remaining, front.quantity_remaining);
        const int64_t fill_price = front.price;

        // If the resting order is fully consumed, remove it from the level and
        // record a fill event for that passive order.
        if (fill_qty == front.quantity_remaining) {
            Order passive = level.pop_front();
            result.quantity_filled += fill_qty;
            qty_remaining -= fill_qty;

            result.fills.push_back(FillEvent{
                aggressive.id,
                passive.id,
                aggressive.symbol,
                aggressive.side,
                fill_price,
                fill_qty,
                timestamp,
                0
            });
            result.consumed_passive_orders.push_back(std::to_string(passive.id));
        } else {
            // If the aggressive order is exhausted mid-order, reduce the front
            // resting order in place and leave it at the head of the queue.
            level.reduce_front(fill_qty);
            result.quantity_filled += fill_qty;
            qty_remaining -= fill_qty;
        }
    }

    // Report whether this level was fully drained by the match.
    result.level_exhausted = level.empty();
    return result;
}

const std::string& FIFOMatcher::name() const noexcept {
    return NAME;
}

} // namespace order_book
