#include "../../include/order_book/sources/interactive_source.hpp"
#include <limits>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

InteractiveSource::InteractiveSource(std::string name)
    : name_(std::move(name))
{}

// ─────────────────────────────────────────────────────────────────────────────
// submit()
//
// Called from the FastAPI / UI thread. Acquires the mutex only long enough to
// push the order and update the atomic flag — O(1), non-blocking.
// ─────────────────────────────────────────────────────────────────────────────

void InteractiveSource::submit(Order order) {
    // Auto-assign an ID if the caller didn't supply one.
    if (order.id == 0) {
        order.id = next_id_.fetch_add(1, std::memory_order_relaxed);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(order));
    }

    // Set after releasing the lock so the engine thread sees a consistent
    // state: has_orders_ == true only after the order is in the queue.
    has_orders_.store(true, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
// submit_cancel()
//
// Cancel requests are stored in a separate cancel_queue_ so they can be
// represented without encoding sentinel values into the Order struct.
// The engine drains cancel_queue_ via next_cancel() after processing orders
// each tick. Cancels submitted alongside orders in the same tick are
// processed in submission order relative to other cancels, but always after
// all pending orders for that tick — this mirrors real exchange semantics
// where a cancel can't affect an order that arrived in the same batch.
// ─────────────────────────────────────────────────────────────────────────────

void InteractiveSource::submit_cancel(uint64_t          order_id,
                                      const std::string& /*symbol*/,
                                      uint64_t           timestamp) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancel_queue_.push({order_id, timestamp});
    }
    has_orders_.store(true, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
// next_cancel()
//
// Called from the engine thread only. Returns the next pending cancel request,
// or nullopt if the cancel queue is empty. Draining the cancel queue is the
// engine's responsibility — it calls this in a loop after each order batch.
// ─────────────────────────────────────────────────────────────────────────────

std::optional<CancelRequest> InteractiveSource::next_cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancel_queue_.empty()) return std::nullopt;
    CancelRequest req = cancel_queue_.front();
    cancel_queue_.pop();
    return req;
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderSource interface
// ─────────────────────────────────────────────────────────────────────────────

uint64_t InteractiveSource::next_timestamp() const noexcept {
    // Lock-free check on the hot path. The engine calls this on every tick.
    // A false positive (has_orders_ true but queue transiently empty) is safe:
    // next_order() will return nullopt and the engine skips this source.
    //
    // Return 0 (lowest possible timestamp) when orders are pending so that
    // interactive orders always have the highest priority in the heap — they
    // represent "right now" relative to any historical or strategy source.
    if (has_orders_.load(std::memory_order_acquire)) {
        return 0;
    }
    return std::numeric_limits<uint64_t>::max();
}

std::optional<Order> InteractiveSource::next_order(uint64_t current_time) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (queue_.empty()) {
        // Update atomic — queue may have been drained by a prior call.
        // Only clear has_orders_ if cancel_queue_ is also empty so the engine
        // still comes back to drain pending cancels.
        if (cancel_queue_.empty()) {
            has_orders_.store(false, std::memory_order_release);
        }
        return std::nullopt;
    }

    Order order = std::move(queue_.front());
    queue_.pop();

    // Assign simulation time if the UI didn't know the clock.
    if (order.timestamp == 0) {
        order.timestamp = current_time;
    }

    // Update atomic only after the pop — the engine may call next_timestamp()
    // from another thread between ticks.
    if (queue_.empty() && cancel_queue_.empty()) {
        has_orders_.store(false, std::memory_order_release);
    }

    return order;
}

// ─────────────────────────────────────────────────────────────────────────────
// Query
// ─────────────────────────────────────────────────────────────────────────────

size_t InteractiveSource::pending_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

size_t InteractiveSource::pending_cancel_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancel_queue_.size();
}

} // namespace order_book