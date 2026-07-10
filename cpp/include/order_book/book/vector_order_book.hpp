#pragma once

#include "order_book_interface.hpp"
#include "array_price_level.hpp"
#include "../matching/matching_algorithm.hpp"
#include <vector>
#include <unordered_map>
#include <memory>
#include <string>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// VectorOrderBook (post-MVP)
//
// Cache-optimised implementation of OrderBookInterface using a pre-allocated
// array indexed by price offset from a reference price, rather than a
// red-black tree.
//
// Motivation:
//   TreeOrderBook's O(log L) operations have a significant constant factor
//   from pointer chasing through the RB-tree nodes, which are scattered
//   across the heap and regularly cause L1/L2 cache misses. For high-
//   frequency workloads on instruments with a known, bounded price range
//   (e.g. equity futures ticking in a $10 band), a flat array indexed by
//   price offset achieves O(1) lookups with near-zero cache miss overhead.
//
// Internal data structure:
//   levels_     std::vector<ArrayPriceLevel>
//                 Pre-allocated for `capacity` ticks on each side.
//                 Index = (price - base_price) / tick_size
//                 The best bid/ask are maintained as cached indices —
//                 not computed by scanning the array on every query.
//
//   best_bid_idx_, best_ask_idx_
//                 Cached indices into levels_, updated on every add/fill/cancel.
//                 best_bid() and best_ask() are thus O(1) like TreeOrderBook,
//                 but without the RB-tree overhead.
//
// Constraints (vs. TreeOrderBook):
//   - Requires a known tick_size and pre-allocated price range.
//   - Prices outside the range cause a reallocation (or throw, depending
//     on the configured overflow policy).
//   - Cancel by order_id is O(N) per level (ArrayPriceLevel limitation).
//     For cancel-heavy workloads, TreeOrderBook is the better choice.
//
// When to use VectorOrderBook:
//   - Throughput benchmarks where fill/add dominates over cancel
//   - Instruments with narrow, predictable price ranges
//   - Historical replay of a single instrument at full speed
//
// When to use TreeOrderBook:
//   - Wide price ranges or unknown tick structure (e.g. crypto)
//   - Cancel-heavy workloads (market making strategies)
//   - MVP / general-purpose use — simpler, no configuration required
//
// NOTE: Post-MVP. The implementation (.cpp) is not part of the MVP build.
//       Gate with cmake option AQUILA_BUILD_VECTOR_BOOK (default OFF for MVP).
// ─────────────────────────────────────────────────────────────────────────────

class VectorOrderBook : public OrderBookInterface {
public:

    struct Config {
        std::string symbol;
        int64_t     base_price;      // basis points — centre of the price array
        int64_t     tick_size;       // basis points per tick
        size_t      half_capacity;   // number of ticks above and below base_price
                                     // total array size = 2 * half_capacity + 1
    };

    explicit VectorOrderBook(Config                             config,
                             std::unique_ptr<MatchingAlgorithm> matcher,
                             OrderBookCallbacks                 callbacks = {});

    // ── OrderBookInterface ────────────────────────────────────────────────────

    void add(Order order)                                          override;
    void cancel(uint64_t order_id)                                 override;
    void execute(uint64_t order_id, uint64_t qty, uint64_t ts)    override;
    void replace(uint64_t old_order_id, Order new_order)          override;

    std::optional<int64_t> best_bid()     const noexcept override;
    std::optional<int64_t> best_ask()     const noexcept override;
    std::optional<int64_t> spread()       const noexcept override;
    std::optional<double>  mid_price()    const noexcept override;
    std::optional<double>  weighted_mid() const noexcept override;

    uint64_t total_bid_qty()                  const noexcept override;
    uint64_t total_ask_qty()                  const noexcept override;
    size_t   bid_depth()                      const noexcept override;
    size_t   ask_depth()                      const noexcept override;
    bool     has_order(uint64_t order_id)     const noexcept override;
    int64_t  last_trade_price()               const noexcept override;

    BookSnapshot snapshot(int depth = 10)     const override;

    const std::string& symbol()              const noexcept override;
    uint64_t           sequence()            const noexcept override;

    void set_callbacks(OrderBookCallbacks callbacks)               override;
    void set_matching_algorithm(
             std::unique_ptr<MatchingAlgorithm> algo)              override;

    // ── VectorOrderBook-specific ──────────────────────────────────────────────

    // Recenter the array around a new base price. All resting orders must be
    // re-indexed. Expensive — avoid calling during active simulation.
    void recenter(int64_t new_base_price);

    const Config& config() const noexcept { return config_; }

private:
    size_t price_to_index(int64_t price) const;
    int64_t index_to_price(size_t idx)   const;
    bool    is_bid_index(size_t idx)     const noexcept;

    void update_best_bid_down();  // called after a bid level becomes empty
    void update_best_ask_up();    // called after an ask level becomes empty
    void match(Order& aggressive);
    bool can_fill_fully(const Order& order) const noexcept;
    void emit_snapshot(uint64_t timestamp);

    Config   config_;
    uint64_t sequence_         = 0;
    int64_t  last_trade_price_ = 0;

    // Flat array: indices [0, half_capacity-1] = bids (descending price)
    //             indices [half_capacity, 2*half_capacity] = asks (ascending price)
    // Conceptually split at base_price; implementation detail exposed here
    // only to allow the recenter() implementation.
    std::vector<ArrayPriceLevel> levels_;

    size_t best_bid_idx_ = 0;     // SIZE_MAX if no bids
    size_t best_ask_idx_ = 0;     // SIZE_MAX if no asks

    // order_id → index into levels_ (cancel still needs a lookup)
    std::unordered_map<uint64_t, size_t> order_index_map_;

    std::unique_ptr<MatchingAlgorithm> matcher_;
    OrderBookCallbacks                 callbacks_;
};

} // namespace order_book