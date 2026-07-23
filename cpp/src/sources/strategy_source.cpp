#include "../../include/order_book/sources/strategy_source.hpp"
#include <limits>
#include <utility>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

StrategySource::StrategySource(StrategyCallback callback, std::string name)
    : callback_(std::move(callback))
    , name_(std::move(name))
{}

// ─────────────────────────────────────────────────────────────────────────────
// OrderSource interface
//
// Single-threaded by contract (engine's run() thread only — see header), so
// unlike InteractiveSource no locking is needed here.
// ─────────────────────────────────────────────────────────────────────────────

uint64_t StrategySource::next_timestamp() const noexcept {
    if (pending_orders_.empty()) {
        return std::numeric_limits<uint64_t>::max();
    }
    // Buffered orders were generated in response to the snapshot at
    // current_ts_ — that's the right timestamp for them to be dispatched at,
    // not 0 (which would misorder them before any historical event).
    return current_ts_;
}

std::optional<SourceEvent> StrategySource::next_order(uint64_t current_time) {
    if (pending_orders_.empty()) {
        return std::nullopt;
    }

    Order order = std::move(pending_orders_.front());
    pending_orders_.pop();

    if (order.timestamp == 0) {
        order.timestamp = current_time;
    }

    return SourceEvent::new_order(std::move(order));
}

// ─────────────────────────────────────────────────────────────────────────────
// on_book_update()
//
// Invokes the strategy callback and buffers whatever orders it returns for
// the NEXT tick (see the one-tick-delay rationale in the header).
// ─────────────────────────────────────────────────────────────────────────────

void StrategySource::on_book_update(const BookSnapshot& snapshot, uint64_t timestamp) {
    if (!callback_) return;

    current_ts_ = timestamp;
    invocations_++;

    std::vector<Order> orders = callback_(snapshot, timestamp);
    for (auto& order : orders) {
        pending_orders_.push(std::move(order));
    }
    total_submitted_ += orders.size();
}

// ─────────────────────────────────────────────────────────────────────────────
// set_callback()
// ─────────────────────────────────────────────────────────────────────────────

void StrategySource::set_callback(StrategyCallback callback) {
    callback_ = std::move(callback);

    // Discard buffered orders from the old callback — see header contract.
    std::queue<Order> empty;
    std::swap(pending_orders_, empty);
}

} // namespace order_book
