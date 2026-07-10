#pragma once

#include "price_level_interface.hpp"
#include "../core/order.hpp"
#include <list>
#include <unordered_map>
#include <cstdint>
#include <stdexcept>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// LinkedListPriceLevel
//
// Doubly-linked list implementation of PriceLevelInterface. This is the MVP
// implementation used by TreeOrderBook.
//
// Internal storage: std::list<Order>
//   - O(1) append to back (new order, time priority)
//   - O(1) pop/erase from front (oldest order filled first)
//   - O(1) erase anywhere given a stored iterator — this is the property that
//     makes cancel O(1) when the OrderBook's order_map stores iterators
//
// Iterator stability: std::list iterators remain valid after any insertion or
// erasure of other elements. This is the key correctness property that makes
// the "hash map of iterators" cancel strategy work. std::vector does NOT
// provide this guarantee — use ArrayPriceLevel for vector-backed books.
//
// Not copyable: copying would silently invalidate all iterators stored in the
// parent OrderBook's order_map. The move constructor is safe because std::list
// move preserves iterator validity.
// ─────────────────────────────────────────────────────────────────────────────

class LinkedListPriceLevel : public PriceLevelInterface {
public:
    using OrderList = std::list<Order>;
    using Iterator  = OrderList::iterator;

    explicit LinkedListPriceLevel(int64_t price);

    // Non-copyable (see class-level comment)
    LinkedListPriceLevel(const LinkedListPriceLevel&)            = delete;
    LinkedListPriceLevel& operator=(const LinkedListPriceLevel&) = delete;

    // Movable — std::map::emplace requires this
    LinkedListPriceLevel(LinkedListPriceLevel&&)            = default;
    LinkedListPriceLevel& operator=(LinkedListPriceLevel&&) = default;

    // ── PriceLevelInterface ───────────────────────────────────────────────────

    void add(Order order)       override;
    void remove(uint64_t order_id) override;
    Order pop_front()           override;
    void reduce_front(uint64_t qty) override;

    bool     empty()       const noexcept override;
    uint64_t total_qty()   const noexcept override;
    uint32_t order_count() const noexcept override;
    int64_t  price()       const noexcept override;

    const Order& front()   const override;

    void visit(const std::function<VisitAction(Order&)>& visitor) override;

    // ── Extended interface (used by TreeOrderBook's order_map) ────────────────

    // Returns a stable iterator to the just-added order after calling add().
    // The OrderBook stores this iterator in order_map_ for O(1) cancel.
    // Must be called immediately after add() before any other mutation.
    Iterator last_added_iterator() noexcept { return last_added_; }

    // O(1) erase given a stored iterator. Called by TreeOrderBook::cancel()
    // which already has the iterator from order_map_.
    void erase(Iterator it);

private:
    int64_t   price_;
    uint64_t  total_qty_ = 0;
    OrderList orders_;
    Iterator  last_added_;   // valid after add(), undefined otherwise
};

} // namespace order_book