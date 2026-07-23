#include "../../include/order_book/book/linked_list_price_level.hpp"
#include <iostream>
#include <stdexcept>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

LinkedListPriceLevel::LinkedListPriceLevel(int64_t price)
    : price_(price)
    , total_qty_(0)
{}

// ─────────────────────────────────────────────────────────────────────────────
// Mutation
// ─────────────────────────────────────────────────────────────────────────────

// adds an order to the back of the list (FIFO)
void LinkedListPriceLevel::add(Order order) {
    total_qty_ += order.quantity_remaining;
    orders_.push_back(std::move(order));
    last_added_ = std::prev(orders_.end()); // iterator to the element we just appended
}

// O(N) scan to find the order by ID and remove it. 
void LinkedListPriceLevel::remove(uint64_t order_id) {
    // Linear scan — called by the interface contract when the caller does NOT
    // have a stored iterator. O(N). In practice, TreeOrderBook always has a
    // stored iterator and calls erase(it) instead, so this path is rarely hit.
    for (auto it = orders_.begin(); it != orders_.end(); ++it) {
        if (it->id == order_id) {
            total_qty_ -= it->quantity_remaining;
            orders_.erase(it);
            return;
        }
    }
    // No-op if not found — matches the interface contract.
}

// O(1) pop from the front of the list (FIFO)
Order LinkedListPriceLevel::pop_front() {
    if (orders_.empty()) {
        throw std::logic_error("LinkedListPriceLevel::pop_front() called on empty level");
    }
    Order front = std::move(orders_.front());
    total_qty_ -= front.quantity_remaining;
    orders_.pop_front();
    return front;
}

// O(1) reduce the front order's quantity (FIFO)
void LinkedListPriceLevel::reduce_front(uint64_t qty) {
    if (orders_.empty()) {
        throw std::logic_error("LinkedListPriceLevel::reduce_front() called on empty level");
    }
    if (qty > orders_.front().quantity_remaining) {
        throw std::logic_error("LinkedListPriceLevel::reduce_front(): qty exceeds front order quantity");
    }
    orders_.front().quantity_remaining -= qty;
    total_qty_              -= qty;
}

// ─────────────────────────────────────────────────────────────────────────────
// O(1) erase by stored iterator (called by TreeOrderBook::cancel)
// ─────────────────────────────────────────────────────────────────────────────

void LinkedListPriceLevel::erase(Iterator it) {
    total_qty_ -= it->quantity_remaining;
    orders_.erase(it);
}

// O(1) reduce an arbitrary order's quantity given a stored iterator.
void LinkedListPriceLevel::reduce(Iterator it, uint64_t qty) {
    if (qty > it->quantity_remaining) {
        throw std::logic_error(
            "LinkedListPriceLevel::reduce(): qty exceeds order quantity");
    }
    it->quantity_remaining -= qty;
    total_qty_             -= qty;
}

// ─────────────────────────────────────────────────────────────────────────────
// Query
// ─────────────────────────────────────────────────────────────────────────────

bool LinkedListPriceLevel::empty() const noexcept {
    return orders_.empty();
}

uint64_t LinkedListPriceLevel::total_qty() const noexcept {
    return total_qty_;
}

uint32_t LinkedListPriceLevel::order_count() const noexcept {
    return static_cast<uint32_t>(orders_.size());
}

int64_t LinkedListPriceLevel::price() const noexcept {
    return price_;
}

const Order& LinkedListPriceLevel::front() const {
    if (orders_.empty()) {
        throw std::logic_error("LinkedListPriceLevel::front() called on empty level");
    }
    return orders_.front();
}

// ─────────────────────────────────────────────────────────────────────────────
// Iteration
//
// The visitor may call pop_front() or reduce_front() on this level during
// traversal. We handle this by advancing the iterator BEFORE invoking the
// visitor — meaning we've already moved past the current element before the
// visitor potentially erases it. This is the standard safe-erase-during-
// iteration pattern for std::list.
// ─────────────────────────────────────────────────────────────────────────────

void LinkedListPriceLevel::visit(
    const std::function<VisitAction(Order&)>& visitor)
{
    auto it = orders_.begin();
    while (it != orders_.end()) {
        // Advance before calling visitor — safe even if visitor calls pop_front(),
        // which erases orders_.front() (== current *it).
        auto current = it++;
        VisitAction action = visitor(*current);
        if (action == VisitAction::STOP) {
            break;
        }
    }
}

} // namespace order_book