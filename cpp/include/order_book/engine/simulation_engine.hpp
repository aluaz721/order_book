#pragma once

#include "simulation_config.hpp"
#include "../book/order_book_interface.hpp"
#include "../orders/stop_order_manager.hpp"
#include "../orders/order_validators.hpp"
#include "../sources/order_source.hpp"
#include "../sources/interactive_source.hpp"
#include "../sources/historical_replayer.hpp"
#include "../sources/strategy_source.hpp"
#include "../core/event.hpp"
#include <memory>
#include <vector>
#include <queue>
#include <functional>
#include <atomic>
#include <string>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// SimulationEngine
//
// Top-level orchestrator. Owns one OrderBook, one StopOrderManager, and any
// number of OrderSources. On each tick it selects the source with the smallest
// next_timestamp() via a priority-queue merge, validates the incoming order,
// routes it to the correct handler (book or stop manager), and notifies all
// sources of the resulting book state.
//
// Priority-queue merge:
//   The engine maintains a min-heap of (next_timestamp, source_index) pairs.
//   On each tick it pops the minimum, calls next_order(), processes the order,
//   re-queries next_timestamp() on that source, and re-pushes it. This
//   guarantees global time-ordering regardless of source types or count.
//
//   If multiple sources have identical timestamps, they are drained in the
//   order they appear in sources_ (i.e. registration order). This is a
//   deterministic tie-breaking policy — important for reproducibility.
//
// Order routing:
//   STOP, STOP_LIMIT  →  StopOrderManager::submit()
//   all others        →  OrderValidators::validate() → OrderBook::add()
//
//   After each fill emitted by the book, the engine calls
//   stop_manager_.on_trade() to check for triggered stops. Triggered stops
//   produce new orders that are submitted synchronously in the same tick
//   (not buffered), to correctly model the cascade of stop triggers.
//
// Feedback to strategies:
//   After each BookSnapshot is emitted by the book, the engine calls
//   on_book_update() on all OrderSources. StrategySource uses this to invoke
//   the Python strategy callback and buffer returned orders for the next tick.
//   Other sources (HistoricalReplayer, InteractiveSource) ignore it.
//
// Thread safety:
//   run() is NOT thread-safe and must be called from a single thread.
//   stop() is thread-safe (atomic store) and may be called from any thread.
//   InteractiveSource::submit() is thread-safe (mutex-protected queue).
//   All other public methods must be called before run() starts or after it
//   returns.
// ─────────────────────────────────────────────────────────────────────────────

class SimulationEngine {
public:

    // ── Construction ──────────────────────────────────────────────────────────

    // `book` must be fully configured (callbacks will be overwritten by the
    // engine to wire itself into the event stream).
    SimulationEngine(SimulationConfig               config,
                     std::unique_ptr<OrderBookInterface> book);

    // ── Source registration (call before run()) ───────────────────────────────

    // Takes ownership of the source. Registration order determines tie-breaking
    // when two sources have identical timestamps.
    void add_source(std::unique_ptr<OrderSource> source);

    // Typed convenience accessors — return nullptr if not registered.
    // Ownership remains with the engine.
    InteractiveSource*  interactive_source()  const noexcept;
    HistoricalReplayer* historical_replayer() const noexcept;
    StrategySource*     strategy_source()     const noexcept;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    // Run the simulation loop synchronously. Blocks until all non-live sources
    // are exhausted and no live source has pending orders, or until stop() is
    // called. Thread: call from the engine thread only.
    void run();

    // Signal the engine to stop after completing the current order.
    // Thread-safe — safe to call from the FastAPI handler or UI thread.
    void stop() noexcept;

    // Reset to initial state: clear the book, reset all sources, reset
    // statistics. Must not be called while run() is executing.
    void reset();

    // ── Callbacks ─────────────────────────────────────────────────────────────
    // Set before calling run(). All are optional (default: no-op).

    // Per-fill notification (forwarded from the book).
    void set_fill_callback(FillCallback cb);

    // Per-cancellation notification (forwarded from book + stop manager).
    void set_cancel_callback(CancelCallback cb);

    // Per-order acknowledgement.
    void set_ack_callback(AckCallback cb);

    // Per-book-state-change snapshot (forwarded from the book).
    void set_snapshot_callback(SnapshotCallback cb);

    // Per-stop-trigger notification.
    void set_trigger_callback(TriggerCallback cb);

    // Called once per engine tick after all sources for that timestamp have
    // been processed. Useful for the Python feature engine to update rolling
    // statistics at tick granularity rather than per-order granularity.
    using TickCallback = std::function<void(uint64_t timestamp,
                                            const BookSnapshot& snapshot)>;
    void set_tick_callback(TickCallback cb);

    // ── State / diagnostics ───────────────────────────────────────────────────

    bool     is_running()        const noexcept { return running_.load(); }
    uint64_t current_time()      const noexcept { return current_time_; }
    uint64_t orders_processed()  const noexcept { return orders_processed_; }
    uint64_t orders_rejected()   const noexcept { return orders_rejected_; }
    uint64_t stops_triggered()   const noexcept { return stops_triggered_; }
    uint64_t tick_count()        const noexcept { return tick_count_; }

    // Direct read-only access to the book for Python inspection via pybind11.
    const OrderBookInterface& book() const noexcept { return *book_; }

    const SimulationConfig& config() const noexcept { return config_; }

private:

    // ── Priority queue ────────────────────────────────────────────────────────

    struct SourceEntry {
        uint64_t next_ts;   // cached next_timestamp() from the source
        size_t   index;     // index into sources_

        bool operator>(const SourceEntry& o) const noexcept {
            if (next_ts != o.next_ts) return next_ts > o.next_ts;
            return index > o.index;   // tie-break: lower index wins
        }
    };

    using SourceHeap = std::priority_queue<
        SourceEntry,
        std::vector<SourceEntry>,
        std::greater<SourceEntry>>;

    // ── Internal helpers ──────────────────────────────────────────────────────

    void rebuild_heap();

    // Route a validated order to the book or stop manager.
    void dispatch(Order order);

    // Wire the engine's own callbacks into the book and stop manager.
    void wire_callbacks();

    // Notify all sources of a new book snapshot.
    void notify_sources(const BookSnapshot& snapshot, uint64_t timestamp);

    // Generate the next unique order ID (used by StopOrderManager and
    // InteractiveSource when id == 0).
    uint64_t next_order_id() noexcept { return next_order_id_counter_++; }

    // ── Members ───────────────────────────────────────────────────────────────

    SimulationConfig config_;

    std::unique_ptr<OrderBookInterface> book_;
    std::unique_ptr<StopOrderManager>   stop_manager_;

    std::vector<std::unique_ptr<OrderSource>> sources_;
    SourceHeap                                heap_;

    uint64_t current_time_         = 0;
    uint64_t orders_processed_     = 0;
    uint64_t orders_rejected_      = 0;
    uint64_t stops_triggered_      = 0;
    uint64_t tick_count_           = 0;
    uint64_t next_order_id_counter_;

    std::atomic<bool> running_{false};

    // Callbacks
    FillCallback     on_fill_;
    CancelCallback   on_cancel_;
    AckCallback      on_ack_;
    SnapshotCallback on_snapshot_;
    TriggerCallback  on_trigger_;
    TickCallback     on_tick_;

    // Most recently emitted snapshot (cached for tick callback and sources)
    BookSnapshot last_snapshot_;
};

} // namespace order_book