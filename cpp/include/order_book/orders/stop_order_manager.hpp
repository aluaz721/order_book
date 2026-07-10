#pragma once

#include "../core/order.hpp"
#include "../core/event.hpp"
#include "../book/order_book_interface.hpp"
#include <map>
#include <unordered_map>
#include <functional>
#include <string>
#include <memory>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// StopOrderManager
//
// Manages pending STOP and STOP_LIMIT orders that are not visible in the main
// order book. Watches the stream of fill events from the OrderBook and
// converts stop orders to live orders when their trigger conditions are met.
//
// Why stops live outside the OrderBook:
//   Stop orders are conditional triggers, not resting orders. They must not
//   appear in the visible book depth or affect spread/mid calculations.
//   Separating them into a dedicated manager keeps the OrderBook's matching
//   logic simple and purely focused on visible, matchable orders.
//
// Internal storage (two trees, separate from the main book):
//
//   stop_buys_   std::map<int64_t, StopQueue>  (ascending by stop price)
//                  Triggered when last_trade_price RISES to or above stop_price.
//                  Used for: BUY stops (buying breakouts), covering short positions.
//                  Best trigger candidate = lowest stop price = map::begin().
//
//   stop_sells_  std::map<int64_t, StopQueue, std::greater<int64_t>>
//                  Triggered when last_trade_price FALLS to or below stop_price.
//                  Used for: SELL stops (cutting losses), stop-loss orders.
//                  Best trigger candidate = highest stop price = map::begin().
//
// Trigger semantics:
//   After every fill in the OrderBook, on_trade() is called with the fill
//   price. StopOrderManager scans the relevant stop tree from the trigger-
//   closest level inward:
//     - stop_buys  triggered if fill_price >= stop_price
//     - stop_sells triggered if fill_price <= stop_price
//   Triggered stops are converted and submitted to the OrderBook immediately.
//   The converted orders may produce additional fills, which may trigger
//   additional stops — this cascade is handled iteratively, not recursively,
//   to avoid stack overflow on pathological inputs.
//
// Converted order types:
//   STOP       → MARKET order at the time of trigger
//   STOP_LIMIT → LIMIT order at the order's limit_price at the time of trigger
//
// Order ID assignment:
//   The triggered order receives a NEW unique ID (generated internally).
//   The TriggerEvent records both the original stop_order_id and the new
//   triggered_order_id so the caller can correlate them.
// ─────────────────────────────────────────────────────────────────────────────

class StopOrderManager {
public:

    // ── Construction ──────────────────────────────────────────────────────────

    // `book` must outlive this StopOrderManager.
    // `on_trigger` is called when a stop order fires, before the converted
    // order is submitted to the book.
    // `on_cancel` is called when a pending stop is explicitly cancelled.
    // `next_order_id` is a shared counter for generating unique IDs for the
    // converted live orders — pass the same counter used by your OrderSource.
    StopOrderManager(OrderBookInterface&    book,
                     TriggerCallback        on_trigger,
                     CancelCallback         on_cancel,
                     std::function<uint64_t()> next_order_id);

    // ── Submission ────────────────────────────────────────────────────────────

    // Accept a STOP or STOP_LIMIT order. Validates type; asserts for others.
    // Emits AckEvent with PENDING_TRIGGER status if accepted, REJECTED if not.
    void submit(Order order, AckCallback on_ack);

    // Cancel a pending stop order by ID.
    // No-op if the order has already been triggered or is unknown.
    void cancel(uint64_t order_id, uint64_t timestamp);

    // ── Trigger check ─────────────────────────────────────────────────────────

    // Called by SimulationEngine after every FillEvent from the OrderBook.
    // Checks both stop trees and triggers all orders whose conditions are now
    // met. Handles cascades iteratively.
    void on_trade(int64_t trade_price, uint64_t timestamp);

    // ── Query ─────────────────────────────────────────────────────────────────

    size_t pending_stop_count()  const noexcept;
    bool   has_stop(uint64_t order_id) const noexcept;

    // Best (most likely to trigger next) stop prices on each side.
    // Returns 0 if no stops pending on that side.
    int64_t nearest_stop_buy()  const noexcept;
    int64_t nearest_stop_sell() const noexcept;

private:

    // ── Internal types ────────────────────────────────────────────────────────

    // Multiple stops may share the same trigger price — stored in FIFO queue.
    using StopQueue = std::vector<Order>;

    using StopBuyMap  = std::map<int64_t, StopQueue>;                     // ascending
    using StopSellMap = std::map<int64_t, StopQueue, std::greater<int64_t>>; // descending

    // ── Internal helpers ──────────────────────────────────────────────────────

    // Convert a triggered stop order to a live order and submit to book_.
    void trigger(Order& stop_order, int64_t trade_price, uint64_t timestamp);

    // Trigger all stops in a queue at a given price level.
    void trigger_queue(StopQueue& queue, int64_t trade_price, uint64_t timestamp);

    // ── Members ───────────────────────────────────────────────────────────────

    OrderBookInterface&          book_;
    TriggerCallback              on_trigger_;
    CancelCallback               on_cancel_;
    std::function<uint64_t()>    next_order_id_;

    StopBuyMap  stop_buys_;
    StopSellMap stop_sells_;

    // order_id → which map + price level the stop lives in
    struct StopLocation {
        Side    side;      // BID = stop_buy, ASK = stop_sell
        int64_t price;     // stop (trigger) price
    };
    std::unordered_map<uint64_t, StopLocation> stop_map_;
};

} // namespace order_book