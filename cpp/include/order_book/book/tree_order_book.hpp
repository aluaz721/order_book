#pragma once

#include "order_book_interface.hpp"
#include "linked_list_price_level.hpp"
#include "../matching/matching_algorithm.hpp"
#include <map>
#include <unordered_map>
#include <functional>
#include <memory>
#include <string>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// TreeOrderBook
//
// MVP implementation of OrderBookInterface using std::map (red-black tree)
// for price-level indexing and LinkedListPriceLevel for per-level order
// queues. This is the classic HowToHFT data structure combination.
//
// Internal data structures:
//
//   bids_     std::map<int64_t, LinkedListPriceLevel, std::greater<int64_t>>
//               RB-tree, descending by price. rbegin() would give the worst
//               bid; begin() gives the BEST bid (highest price). O(log L).
//
//   asks_     std::map<int64_t, LinkedListPriceLevel>
//               RB-tree, ascending by price. begin() gives the BEST ask
//               (lowest price). O(log L).
//
//   order_map_ std::unordered_map<uint64_t, OrderLocation>
//               Maps order_id → {side, price, list_iterator}. Enables O(1)
//               cancel without scanning price levels. This is the critical
//               structure that separates a fast LOB from a slow one.
//
// L = number of distinct price levels (typically 10–500 in practice).
//
// Complexity:
//   add()          O(log L) amortized (map lookup or insert + list append)
//   cancel()       O(log L) (hash map O(1) + list erase O(1) + map erase O(log L))
//   execute()      O(log L)
//   best_bid/ask() O(1)     (map::begin())
//   snapshot()     O(depth) (iterate top-depth map entries)
//
// Stop/stop-limit orders: NOT handled here. Route through StopOrderManager.
// ─────────────────────────────────────────────────────────────────────────────

class TreeOrderBook : public OrderBookInterface {
public:

    // ── Construction ──────────────────────────────────────────────────────────

    TreeOrderBook(std::string                        symbol,
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

private:

    // ── Internal types ────────────────────────────────────────────────────────

    // Stores everything needed to locate and erase a resting order in O(1).
    struct OrderLocation {
        Side                            side;
        int64_t                         price;  // which price level map to look in
        LinkedListPriceLevel::Iterator  it;     // stable iterator into the list
    };

    using BidMap = std::map<int64_t, LinkedListPriceLevel, std::greater<int64_t>>;
    using AskMap = std::map<int64_t, LinkedListPriceLevel>;

    // ── Internal helpers ──────────────────────────────────────────────────────

    // Attempt to match `aggressive` against the opposite side.
    // Modifies aggressive.quantity in place. Fires on_fill_ per match.
    // Cleans up empty price levels after matching.
    void match(Order& aggressive);

    // Check if a FOK order can be fully filled without modifying book state.
    bool can_fill_fully(const Order& order) const noexcept;

    // Insert a resting order into the correct price level (creating the
    // level if needed) and record it in order_map_.
    void rest(Order order);

    // Remove a price level from its map if it has become empty.
    void prune_level(Side side, int64_t price);

    // Emit a BookSnapshot via on_book_update_ and increment sequence_.
    void emit_snapshot(uint64_t timestamp);

    // ── Members ───────────────────────────────────────────────────────────────

    std::string symbol_;
    uint64_t    sequence_          = 0;
    int64_t     last_trade_price_  = 0;

    BidMap bids_;
    AskMap asks_;

    std::unordered_map<uint64_t, OrderLocation> order_map_;

    std::unique_ptr<MatchingAlgorithm> matcher_;
    OrderBookCallbacks                 callbacks_;
};

} // namespace order_book