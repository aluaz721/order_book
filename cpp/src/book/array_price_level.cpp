#include "../../include/order_book/book/array_price_level.hpp"
#include <stdexcept>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// NOTE: Post-MVP implementation.
// This file is compiled only when AQUILA_BUILD_VECTOR_BOOK=ON.
// The stub implementations below are sufficient to link; replace with
// production implementations before enabling VectorOrderBook in benchmarks.
// ─────────────────────────────────────────────────────────────────────────────

namespace order_book {

// Compact when more than 25% of the array is dead head
static constexpr double COMPACT_THRESHOLD = 0.25;

ArrayPriceLevel::ArrayPriceLevel(int64_t price, size_t initial_capacity)
    : price_(price)
    , total_qty_(0)
    , head_(0)
{
    orders_.reserve(initial_capacity);
}

// ─────────────────────────────────────────────────────────────────────────────
// Mutation
// ─────────────────────────────────────────────────────────────────────────────

void ArrayPriceLevel::add(Order order) {
    total_qty_ += order.quantity_remaining;
    orders_.push_back(std::move(order));
}

void ArrayPriceLevel::remove(uint64_t order_id) {
    // O(N) scan from head — no stored iterator available for array-backed levels.
    for (size_t i = head_; i < orders_.size(); ++i) {
        if (orders_[i].id == order_id) {
            total_qty_ -= orders_[i].quantity_remaining;
            // Swap-erase: swap with last live element to avoid shifting.
            // This breaks time priority for the swapped element — callers using
            // VectorOrderBook must be aware that remove() disturbs FIFO order
            // after the removed position. For pro-rata matching this is acceptable.
            if (i != orders_.size() - 1) {
                std::swap(orders_[i], orders_.back());
            }
            orders_.pop_back();
            return;
        }
    }
    // No-op if not found.
}

Order ArrayPriceLevel::pop_front() {
    if (head_ >= orders_.size()) {
        throw std::logic_error("ArrayPriceLevel::pop_front() called on empty level");
    }
    Order front     = std::move(orders_[head_]);
    total_qty_     -= front.quantity_remaining;
    head_++;
    maybe_compact();
    return front;
}

void ArrayPriceLevel::reduce_front(uint64_t qty) {
    if (head_ >= orders_.size()) {
        throw std::logic_error("ArrayPriceLevel::reduce_front() called on empty level");
    }
    if (qty > orders_[head_].quantity_remaining) {
        throw std::logic_error("ArrayPriceLevel::reduce_front(): qty exceeds front order quantity");
    }
    orders_[head_].quantity_remaining -= qty;
    total_qty_              -= qty;
}

// ─────────────────────────────────────────────────────────────────────────────
// Query
// ─────────────────────────────────────────────────────────────────────────────

bool ArrayPriceLevel::empty() const noexcept {
    return head_ >= orders_.size();
}

uint64_t ArrayPriceLevel::total_qty() const noexcept {
    return total_qty_;
}

uint32_t ArrayPriceLevel::order_count() const noexcept {
    return static_cast<uint32_t>(orders_.size() - head_);
}

int64_t ArrayPriceLevel::price() const noexcept {
    return price_;
}

const Order& ArrayPriceLevel::front() const {
    if (head_ >= orders_.size()) {
        throw std::logic_error("ArrayPriceLevel::front() called on empty level");
    }
    return orders_[head_];
}

// ─────────────────────────────────────────────────────────────────────────────
// Iteration
// ─────────────────────────────────────────────────────────────────────────────

void ArrayPriceLevel::visit(
    const std::function<VisitAction(Order&)>& visitor)
{
    // Walk live elements from head forward. The visitor may call pop_front()
    // which increments head_ — we advance our local index independently to
    // avoid double-visiting or skipping elements.
    size_t i = head_;
    while (i < orders_.size()) {
        VisitAction action = visitor(orders_[i]);
        i++;
        if (action == VisitAction::STOP) break;
    }
    maybe_compact();
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal: lazy compaction
// ─────────────────────────────────────────────────────────────────────────────

void ArrayPriceLevel::maybe_compact() {
    if (orders_.empty()) return;
    double dead_ratio = static_cast<double>(head_) / static_cast<double>(orders_.size());
    if (dead_ratio > COMPACT_THRESHOLD && head_ > 0) {
        orders_.erase(orders_.begin(),
                      orders_.begin() + static_cast<ptrdiff_t>(head_));
        head_ = 0;
    }
}

} // namespace order_book