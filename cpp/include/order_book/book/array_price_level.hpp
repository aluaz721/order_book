#pragma once

#include "price_level_interface.hpp"
#include "../core/order.hpp"
#include <vector>
#include <cstdint>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// ArrayPriceLevel (post-MVP)
//
// Vector-backed implementation of PriceLevelInterface for use with
// VectorOrderBook. Designed for cache-line-friendly sequential access at the
// cost of O(N) removal.
//
// Internal storage: std::vector<Order>
//   - Orders stored contiguously in memory → better cache behavior during
//     sequential fill sweeps compared to LinkedListPriceLevel
//   - O(1) push_back for new orders
//   - O(1) removal from front (index bump, no erase needed)
//   - O(N) removal by order_id (linear scan + swap-erase)
//
// Trade-off vs. LinkedListPriceLevel:
//   - Cancel by order_id: O(N) instead of O(1) — worse for cancel-heavy
//     workloads (HFT market making), but this is acceptable for the vector
//     book which targets throughput benchmarks over cancel latency
//   - Fill sweep: better cache locality → lower constant factor for the
//     common case of sweeping through a full price level
//
// Because iterators into std::vector are invalidated by any insertion or
// erasure, the "store iterators in order_map" trick used by TreeOrderBook
// does NOT apply. VectorOrderBook must store order indices or use a different
// cancel strategy (e.g. a lazy deletion bitmap).
//
// NOTE: This is a post-MVP class. The declaration is provided now to establish
// the interface contract and allow benchmarks to be written against the
// abstract PriceLevelInterface, but the implementation (.cpp) is not part of
// the MVP build. Mark with AQUILA_EXPERIMENTAL if you gate builds.
// ─────────────────────────────────────────────────────────────────────────────

class ArrayPriceLevel : public PriceLevelInterface {
public:
    explicit ArrayPriceLevel(int64_t price, size_t initial_capacity = 64);

    ArrayPriceLevel(const ArrayPriceLevel&)            = default;
    ArrayPriceLevel& operator=(const ArrayPriceLevel&) = default;
    ArrayPriceLevel(ArrayPriceLevel&&)                 = default;
    ArrayPriceLevel& operator=(ArrayPriceLevel&&)      = default;

    // ── PriceLevelInterface ───────────────────────────────────────────────────

    void  add(Order order)          override;
    void  remove(uint64_t order_id) override;
    Order pop_front()               override;
    void  reduce_front(uint64_t qty)override;

    bool     empty()       const noexcept override;
    uint64_t total_qty()   const noexcept override;
    uint32_t order_count() const noexcept override;
    int64_t  price()       const noexcept override;

    const Order& front()   const override;

    void visit(const std::function<VisitAction(Order&)>& visitor) override;

private:
    int64_t             price_;
    uint64_t            total_qty_  = 0;
    size_t              head_       = 0;   // index of the current front order
    std::vector<Order>  orders_;

    // Compact the vector when the dead-head ratio exceeds a threshold.
    // Called lazily after pop_front() to amortize compaction cost.
    void maybe_compact();
};

} // namespace order_book