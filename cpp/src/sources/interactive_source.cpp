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
// push the event and update the atomic flag — O(1), non-blocking.
// ─────────────────────────────────────────────────────────────────────────────

void InteractiveSource::submit(Order order) {
    // Auto-assign an ID if the caller didn't supply one.
    if (order.id == 0) {
        order.id = next_id_.fetch_add(1, std::memory_order_relaxed);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(SourceEvent::new_order(std::move(order)));
    }

    // Set after releasing the lock so the engine thread sees a consistent
    // state: has_orders_ == true only after the event is in the queue.
    has_orders_.store(true, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
// submit_cancel()
//
// Cancels are just another SourceEvent, queued alongside new orders in
// submission order. The engine routes SourceEventType::CANCEL to
// book.cancel() generically — no separate drain path needed.
// ─────────────────────────────────────────────────────────────────────────────

void InteractiveSource::submit_cancel(uint64_t          order_id,
                                      const std::string& /*symbol*/,
                                      uint64_t           timestamp) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(SourceEvent::cancel(order_id, timestamp));
    }
    has_orders_.store(true, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderSource interface
// ─────────────────────────────────────────────────────────────────────────────

uint64_t InteractiveSource::next_timestamp() const noexcept {
    // Lock-free check on the hot path. The engine calls this on every tick.
    // A false positive (has_orders_ true but queue transiently empty) is safe:
    // next_order() will return nullopt and the engine skips this source.
    //
    // Return 0 (lowest possible timestamp) when events are pending so that
    // interactive orders always have the highest priority in the heap — they
    // represent "right now" relative to any historical or strategy source.
    if (has_orders_.load(std::memory_order_acquire)) {
        return 0;
    }
    return std::numeric_limits<uint64_t>::max();
}

std::optional<SourceEvent> InteractiveSource::next_order(uint64_t current_time) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (queue_.empty()) {
        // Update atomic — queue may have been drained by a prior call.
        has_orders_.store(false, std::memory_order_release);
        return std::nullopt;
    }

    SourceEvent event = std::move(queue_.front());
    queue_.pop();

    // Assign simulation time if the UI didn't know the clock.
    if (event.timestamp == 0) {
        event.timestamp = current_time;
        if (event.type == SourceEventType::NEW_ORDER) {
            event.order.timestamp = current_time;
        }
    }

    // Update atomic only after the pop — the engine may call next_timestamp()
    // from another thread between ticks.
    if (queue_.empty()) {
        has_orders_.store(false, std::memory_order_release);
    }

    return event;
}

// ─────────────────────────────────────────────────────────────────────────────
// Query
// ─────────────────────────────────────────────────────────────────────────────

size_t InteractiveSource::pending_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

} // namespace order_book
