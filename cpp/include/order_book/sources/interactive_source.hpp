#pragma once

#include "order_source.hpp"
#include "../core/order.hpp"
#include <queue>
#include <mutex>
#include <atomic>
#include <optional>
#include <string>
#include <cstdint>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// InteractiveSource
//
// Thread-safe order queue for user-submitted orders from the React UI.
//
// Concurrency model:
//   - submit() is called from a FastAPI request-handler thread. It acquires
//     mutex_, pushes the order, sets has_orders_ (atomic), and returns.
//     O(1), never blocks the HTTP handler for more than a few nanoseconds.
//
//   - next_order() is called from the engine's single run() thread. It
//     acquires mutex_, pops the front order if the queue is non-empty, and
//     releases. O(1).
//
//   - next_timestamp() checks has_orders_ (std::atomic<bool>) WITHOUT locking.
//     The engine calls this on every tick — it must be lock-free. A false
//     positive (has_orders_ true but queue is transiently empty due to a race)
//     is safe: next_order() will return nullopt and the engine skips this
//     source for this tick.
//
// Order ID assignment:
//   If an order's id field is 0, InteractiveSource assigns it a unique ID
//   from an internal atomic counter before enqueuing. This lets the React UI
//   submit orders without managing IDs, while ensuring all IDs in the engine
//   are unique.
//
// Timestamp assignment:
//   If an order's timestamp field is 0, the engine will assign it the current
//   simulation clock time when it drains the order via next_order(). This
//   ensures interactive orders integrate cleanly with a historical replay
//   timeline without requiring the UI to know the simulation clock.
//
// This source is never exhausted — it runs until the engine is stopped.
// ─────────────────────────────────────────────────────────────────────────────

// Plain struct representing a pending cancel request from the UI.
// Kept separate from the Order queue so no sentinel values are needed.
struct CancelRequest {
    uint64_t order_id;
    uint64_t timestamp;  // 0 = use current simulation time
};

class InteractiveSource : public OrderSource {
public:
    explicit InteractiveSource(std::string name = "InteractiveSource");

    // ── Thread-safe submission (called from UI/HTTP thread) ───────────────────

    // Enqueue an order for the engine to process. Thread-safe.
    // If order.id == 0, assigns a unique ID before enqueuing.
    // If order.timestamp == 0, the engine will assign simulation time on drain.
    void submit(Order order);

    // Convenience overload for submitting a cancel request.
    // The engine will call book.cancel(order_id) on the next tick.
    void submit_cancel(uint64_t order_id, const std::string& symbol,
                       uint64_t timestamp = 0);

    // ── OrderSource interface (called from engine thread) ─────────────────────

    uint64_t             next_timestamp() const noexcept override;
    std::optional<Order> next_order(uint64_t current_time) override;
    bool                 exhausted()      const noexcept override { return false; }
    const std::string&   name()           const noexcept override { return name_; }

    // InteractiveSource does not react to book updates — the UI pulls
    // snapshots via WebSocket instead. Override is intentionally a no-op.
    void on_book_update(const BookSnapshot&, uint64_t) override {}

    // ── Query (approximate — may be stale by the time caller reads it) ───────

    size_t pending_count()        const noexcept;
    size_t pending_cancel_count() const noexcept;

    // Drain one cancel request. Called by the engine after each order batch.
    // Engine thread only.
    std::optional<CancelRequest> next_cancel();
    bool   is_empty()      const noexcept { return !has_orders_.load(); }

private:
    std::string              name_;
    mutable std::mutex       mutex_;
    std::queue<Order>         queue_;
    std::queue<CancelRequest> cancel_queue_;
    std::atomic<bool>        has_orders_{false};
    std::atomic<uint64_t>    next_id_{1};    // auto-assigned IDs start at 1
};

} // namespace order_book