#pragma once

#include "../core/order.hpp"
#include <cstdint>
#include <functional>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// PriceLevelInterface
//
// Abstract interface for a collection of resting orders at a single price
// point on one side of the book. Concrete implementations differ in their
// internal storage (doubly-linked list for the MVP tree-based book, flat
// array for the future vector-based book), but expose the same interface to
// the matching algorithm layer above them.
//
// Iteration contract for MatchingAlgorithm:
//   The visit() method calls a visitor for each resting order in the order
//   the implementation deems correct for its allocation semantics. For FIFO
//   this is insertion order (front-to-back); for array-based implementations
//   it may differ. The matcher calls visit() and accumulates fills without
//   knowing or caring about the underlying storage.
//
// Mutation during iteration:
//   Removing the currently-visited order (via remove_front() or a similar
//   call) during a visit() traversal is explicitly supported. Implementations
//   must guarantee this is safe. The visitor returns a VisitAction to signal
//   whether to continue or stop the traversal.
// ─────────────────────────────────────────────────────────────────────────────

class PriceLevelInterface {
public:
    virtual ~PriceLevelInterface() = default;

    // ── Mutation ─────────────────────────────────────────────────────────────

    // Append order to the level. Implementations define position semantics
    // (back of FIFO queue for linked-list; ordered insertion for sorted array).
    virtual void add(Order order) = 0;

    // Remove a specific order by ID. O(N) for naive implementations; O(1) for
    // linked-list implementations that store iterators in the order_map.
    // No-op if order_id is not present at this level.
    virtual void remove(uint64_t order_id) = 0;

    // Remove and return the front order (oldest in FIFO, highest-priority in
    // pro-rata). Called by the matcher after a full fill of the front order.
    // Undefined behaviour if called on an empty level — check empty() first.
    virtual Order pop_front() = 0;

    // Reduce the front order's quantity by `qty`. Called for partial fills.
    // Precondition: qty <= front().quantity.
    virtual void reduce_front(uint64_t qty) = 0;

    // ── Query ─────────────────────────────────────────────────────────────────

    virtual bool     empty()       const noexcept = 0;
    virtual uint64_t total_qty()   const noexcept = 0;
    virtual uint32_t order_count() const noexcept = 0;
    virtual int64_t  price()       const noexcept = 0;

    // Non-mutating access to the front order. Undefined if empty().
    virtual const Order& front()   const = 0;

    // ── Iteration ─────────────────────────────────────────────────────────────

    // Control flow signal returned by the visitor passed to visit().
    enum class VisitAction { CONTINUE, STOP };

    // Walk resting orders in priority order, calling visitor(order) for each.
    // The visitor returns VisitAction::STOP to terminate the traversal early.
    // Implementations must support removal of the currently-visited order
    // (via pop_front / reduce_front) within the visitor body.
    virtual void visit(
        const std::function<VisitAction(Order&)>& visitor) = 0;
};

} // namespace order_book