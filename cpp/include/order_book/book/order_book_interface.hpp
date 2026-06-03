#pragma once

#include "../core/order.hpp"
#include "../core/event.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <memory>

namespace order_book {

// Forward declaration — concrete books need a MatchingAlgorithm but we don't
// want to pull in the full matching/ headers here.
class MatchingAlgorithm;

// ─────────────────────────────────────────────────────────────────────────────
// OrderBookCallbacks
//
// Grouped into a single struct so concrete implementations don't need separate
// constructor parameters for each callback, and so the SimulationEngine can
// register / update all callbacks in one call.
// ─────────────────────────────────────────────────────────────────────────────

struct OrderBookCallbacks {
    FillCallback     on_fill;          // called once per match
    CancelCallback   on_cancel;        // called on any cancellation
    AckCallback      on_ack;           // called once per submitted order
    SnapshotCallback on_book_update;   // called after every state change
};

// ─────────────────────────────────────────────────────────────────────────────
// OrderBookInterface
//
// Abstract base for all limit order book implementations. Concrete classes
// (TreeOrderBook, VectorOrderBook) inherit from this and differ only in their
// internal storage and lookup strategies. The MatchingAlgorithm is injected
// at construction time and is also substitutable — see matching/.
//
// Design principles:
//   1. The interface describes WHAT the book does; implementations define HOW.
//   2. Stop/stop-limit orders are NOT handled here — they live in
//      StopOrderManager, which wraps a concrete OrderBook and converts
//      triggered stops into live orders before calling add().
//   3. All notifications flow exclusively through OrderBookCallbacks.
//      Implementations must not write to stdout or throw across callbacks.
//   4. Thread safety: NOT guaranteed. The SimulationEngine serialises all
//      calls. InteractiveSource uses an external queue to avoid concurrent
//      mutations — it never calls add() directly from a UI thread.
//
// Implementer checklist:
//   - Call on_ack   once per add() call (either NEW or REJECTED).
//   - Call on_fill  once per matched order pair inside add().
//   - Call on_cancel for IOC remainders, FOK failures, explicit cancels.
//   - Call on_book_update once at the end of add() and cancel().
//   - Never call any callback from within a callback (no re-entrancy).
// ─────────────────────────────────────────────────────────────────────────────

class OrderBookInterface {
public:

    virtual ~OrderBookInterface() = default;

    // ── Core operations ───────────────────────────────────────────────────────

    // Process an incoming order. Matching is attempted immediately against the
    // opposite side. Behaviour after matching:
    //   LIMIT:               unfilled remainder rests in the book
    //   MARKET:              unfilled remainder is cancelled
    //   MARKET_LIMIT:        unfilled remainder rests at last traded price
    //   IMMEDIATE_OR_CANCEL: unfilled remainder is cancelled
    //   FILL_OR_KILL:        feasibility pre-checked; cancelled if not fully
    //                        fillable without any partial fills produced
    //   STOP / STOP_LIMIT:   must NOT be passed to add() directly — route
    //                        through StopOrderManager instead
    virtual void add(Order order) = 0;

    // Cancel a resting order by ID. No-op if the order is unknown (already
    // filled or never submitted). Fires on_cancel and on_book_update.
    virtual void cancel(uint64_t order_id) = 0;

    // Partially or fully execute a resting order as reported by an external
    // feed (e.g. ITCH Execute message for a fill driven by a counterparty
    // we're not modelling). Fires on_fill and on_book_update.
    // If qty >= resting quantity, the order is fully removed.
    virtual void execute(uint64_t order_id, uint64_t qty, uint64_t timestamp) = 0;

    // Atomically cancel old_order_id and add new_order. The new order loses
    // time priority — it goes to the back of its price level queue.
    // Equivalent to cancel() + add() but emits a single on_book_update.
    virtual void replace(uint64_t old_order_id, Order new_order) = 0;

    // ── Query ─────────────────────────────────────────────────────────────────

    virtual std::optional<int64_t> best_bid()     const noexcept = 0;
    virtual std::optional<int64_t> best_ask()     const noexcept = 0;
    virtual std::optional<int64_t> spread()       const noexcept = 0;  // ask - bid
    virtual std::optional<double>  mid_price()    const noexcept = 0;  // (bid+ask)/2
    virtual std::optional<double>  weighted_mid() const noexcept = 0;  // qty-weighted

    virtual uint64_t total_bid_qty()    const noexcept = 0;
    virtual uint64_t total_ask_qty()    const noexcept = 0;
    virtual size_t   bid_depth()        const noexcept = 0;  // distinct price levels
    virtual size_t   ask_depth()        const noexcept = 0;
    virtual bool     has_order(uint64_t order_id) const noexcept = 0;

    virtual int64_t  last_trade_price() const noexcept = 0;  // 0 if no trades yet

    // Full snapshot of top-depth levels on each side.
    // depth == -1 returns all levels.
    virtual BookSnapshot snapshot(int depth = 10) const = 0;

    virtual const std::string& symbol()   const noexcept = 0;
    virtual uint64_t           sequence() const noexcept = 0;

    // ── Configuration ─────────────────────────────────────────────────────────

    // Replace callbacks after construction (e.g. when the SimulationEngine
    // wires itself up after creating the book).
    virtual void set_callbacks(OrderBookCallbacks callbacks) = 0;

    // Replace the matching algorithm at runtime. Useful for benchmarks that
    // want to hot-swap FIFO vs. pro-rata without reconstructing the book.
    // Precondition: the book must be empty (undefined behaviour otherwise).
    virtual void set_matching_algorithm(
        std::unique_ptr<MatchingAlgorithm> algo) = 0;
};

} // namespace order_book