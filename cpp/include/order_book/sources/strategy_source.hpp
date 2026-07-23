#pragma once

#include "order_source.hpp"
#include "../core/order.hpp"
#include "../core/event.hpp"
#include <functional>
#include <queue>
#include <string>
#include <vector>
#include <cstdint>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// StrategySource (post-MVP)
//
// Bridges the Python alpha model and the C++ engine. Holds a C++ callable
// (set from Python via pybind11) that is invoked on each book update and
// returns a list of orders for the engine to process.
//
// Feedback loop design:
//   1. Engine processes a batch of historical/interactive events.
//   2. Engine emits a BookSnapshot and calls on_book_update() on all sources.
//   3. StrategySource::on_book_update() invokes strategy_callback_ with the
//      snapshot, gets back a list of Orders.
//   4. Orders are buffered in pending_orders_.
//   5. On the NEXT engine tick, the engine sees StrategySource has pending
//      orders (next_timestamp() != UINT64_MAX) and drains them via next_order().
//
// The one-tick delay (step 4→5) is deliberate: it prevents the strategy from
// filling against the same book state it just observed, which would create
// unrealistic circular fills within a single timestamp. Real strategies have
// at least some latency between signal observation and order submission.
//
// Callback signature:
//   The callback receives a BookSnapshot and the current simulation timestamp,
//   and returns zero or more Orders. Orders may be of any type (including
//   STOP and STOP_LIMIT — the engine routes them to StopOrderManager).
//
//   In Python (via pybind11):
//     def strategy(snapshot: BookSnapshot, timestamp: int) -> list[Order]:
//         ...
//         return [Order(...)]
//
// Thread safety:
//   strategy_callback_ is invoked synchronously on the engine's run() thread.
//   The callback must not block, acquire locks, or call back into the engine.
//
// NOTE: Post-MVP. Built under AQUILA_BUILD_PYTHON_BINDINGS.
// ─────────────────────────────────────────────────────────────────────────────

class StrategySource : public OrderSource {
public:

    using StrategyCallback = std::function<
        std::vector<Order>(const BookSnapshot&, uint64_t timestamp)>;

    explicit StrategySource(StrategyCallback callback,
                            std::string      name = "StrategySource");

    // ── OrderSource interface ─────────────────────────────────────────────────

    uint64_t                    next_timestamp() const noexcept override;
    std::optional<SourceEvent>  next_order(uint64_t current_time) override;
    bool                        exhausted()      const noexcept override { return false; }
    const std::string&          name()           const noexcept override { return name_; }

    // Invokes strategy_callback_ and buffers returned orders.
    // Called by the engine after each BookSnapshot is emitted.
    void on_book_update(const BookSnapshot& snapshot,
                        uint64_t            timestamp) override;

    // ── Query ─────────────────────────────────────────────────────────────────

    size_t pending_order_count() const noexcept { return pending_orders_.size(); }

    // Number of times the strategy callback has been invoked.
    uint64_t callback_invocations() const noexcept { return invocations_; }

    // Number of orders the strategy has submitted in total.
    uint64_t total_orders_submitted() const noexcept { return total_submitted_; }

    // ── Runtime replacement ───────────────────────────────────────────────────

    // Swap in a new strategy callback without stopping the engine.
    // All buffered pending orders from the old callback are discarded.
    // Safe to call from the engine thread only.
    void set_callback(StrategyCallback callback);

private:
    StrategyCallback  callback_;
    std::string       name_;
    std::queue<Order> pending_orders_;   // orders buffered for next tick
    uint64_t          current_ts_   = 0; // timestamp of the last on_book_update call
    uint64_t          invocations_  = 0;
    uint64_t          total_submitted_ = 0;
};

} // namespace order_book