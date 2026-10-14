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
//   Every fill in the OrderBook is reported with record_trade(); once the
//   book operation that produced it has returned, drain_triggers() scans the
//   relevant stop tree from the trigger-closest level inward:
//     - stop_buys  triggered if fill_price >= stop_price
//     - stop_sells triggered if fill_price <= stop_price
//   Triggered stops are converted and submitted to the OrderBook there. The
//   converted orders may produce additional fills, which may trigger
//   additional stops — this cascade is handled iteratively inside the one
//   draining loop, not recursively, both to avoid stack overflow on
//   pathological inputs and because re-entering the book mid-match would
//   corrupt the sweep in progress.
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
    //
    // Triggering is split into "record" and "drain" because a triggered stop
    // is submitted to the book, and the trade that triggered it is reported
    // from inside a book operation. Submitting from the fill callback would
    // re-enter OrderBookInterface::add() while it is still sweeping price
    // levels — the outer sweep would then prune a level the nested call had
    // already erased. So trades are recorded as they are reported and acted
    // on once the book operation has returned.

    // Record a trade for trigger evaluation. Submits nothing, touches no book
    // state — safe to call from an on_fill callback.
    void record_trade(int64_t trade_price, uint64_t timestamp);

    // Evaluate every recorded trade and submit the orders their stops convert
    // into. Call only when the book is not mid-operation, i.e. after
    // add() / cancel() / execute() / replace() has returned.
    //
    // The converted orders may themselves produce fills, which record further
    // trades; those are drained by the same call, so a cascade of stops is
    // processed iteratively here rather than recursively through the book.
    void drain_triggers();

    // record_trade() + drain_triggers(). Convenience for callers that are not
    // inside a book callback.
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

    // A trade reported by the book, awaiting trigger evaluation.
    struct PendingTrade {
        int64_t  price;
        uint64_t timestamp;
    };

    // ── Internal helpers ──────────────────────────────────────────────────────

    // Trigger every stop whose condition is met by one trade price.
    void trigger_for_trade(int64_t trade_price, uint64_t timestamp);

    // Convert a triggered stop order to a live order and submit to book_.
    // Precondition: the caller has already removed `stop_order` from both the
    // stop tree and stop_map_ — see the definition for why.
    void trigger(Order& stop_order, int64_t trade_price, uint64_t timestamp);

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

    // Trades recorded but not yet evaluated, and the guard that keeps a
    // cascade in the single draining loop instead of nesting.
    std::vector<PendingTrade> pending_trades_;
    bool                      draining_ = false;
};

} // namespace order_book