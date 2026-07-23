#include "../../include/order_book/book/tree_order_book.hpp"
#include "../../include/order_book/matching/matching_algorithm.hpp"
#include <stdexcept>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

TreeOrderBook::TreeOrderBook(std::string                        symbol,
                             std::unique_ptr<MatchingAlgorithm> matcher,
                             OrderBookCallbacks                 callbacks)
    : symbol_(std::move(symbol))
    , matcher_(std::move(matcher))
    , callbacks_(std::move(callbacks))
{}

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

void TreeOrderBook::set_callbacks(OrderBookCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

void TreeOrderBook::set_matching_algorithm(
    std::unique_ptr<MatchingAlgorithm> algo)
{
    if (!order_map_.empty()) {
        throw std::logic_error(
            "TreeOrderBook::set_matching_algorithm() called on non-empty book. "
            "Clear all orders before swapping the matching algorithm.");
    }
    matcher_ = std::move(algo);
}

// ─────────────────────────────────────────────────────────────────────────────
// add()
//
// Entry point for every incoming order. The sequence is:
//   1. Emit ACK (NEW)
//   2. Attempt to match against opposite side
//   3. Handle post-match semantics per order type
//   4. Emit book snapshot
// ─────────────────────────────────────────────────────────────────────────────

void TreeOrderBook::add(Order order) {
    // ── 1. Acknowledge receipt ────────────────────────────────────────────────
    if (callbacks_.on_ack) {
        callbacks_.on_ack(AckEvent{
            order.id, symbol_, OrderStatus::NEW, {}, order.timestamp
        });
    }

    // ── 2. Match against opposite side ────────────────────────────────────────
    if (order.type == OrderType::FILL_OR_KILL && !can_fill_fully(order)) {
        if (callbacks_.on_cancel) {
            callbacks_.on_cancel(CancelEvent{
                order.id, symbol_, order.side, order.price,
                CancelReason::FOK_FAILED,
                order.quantity_remaining, order.timestamp, sequence_
            });
        }
        emit_snapshot(order.timestamp);
        return;
    }

    match(order);

    // ── 3. Post-match handling by order type ──────────────────────────────────
    if (order.quantity_remaining > 0) {
        switch (order.type) {

        case OrderType::LIMIT:
            // Rest unfilled remainder in the book.
            order.status = (order.orig_quantity != order.quantity_remaining)
                           ? OrderStatus::PARTIALLY_FILLED
                           : OrderStatus::NEW;
            rest(std::move(order));
            break;

        case OrderType::MARKET:
        case OrderType::IMMEDIATE_OR_CANCEL:
            // Cancel any unfilled remainder — never rests.
            if (callbacks_.on_cancel) {
                CancelEvent evt;
                evt.order_id           = order.id;
                evt.symbol             = symbol_;
                evt.side               = order.side;
                evt.price              = order.price;
                evt.reason             = (order.type == OrderType::MARKET)
                                         ? CancelReason::MARKET_NO_LIQUIDITY
                                         : CancelReason::IOC_EXPIRED;
                evt.remaining_quantity = order.quantity_remaining;
                evt.timestamp          = order.timestamp;
                evt.sequence           = sequence_;
                callbacks_.on_cancel(evt);
            }
            break;

        case OrderType::MARKET_LIMIT:
            // Convert unfilled remainder to a LIMIT order at the last trade
            // price. If no trades have happened yet (last_trade_price_ == 0),
            // fall back to MARKET cancel semantics — there's no sensible
            // price to rest at.
            if (last_trade_price_ > 0) {
                order.type  = OrderType::LIMIT;
                order.price = last_trade_price_;
                order.status = (order.orig_quantity != order.quantity_remaining)
                               ? OrderStatus::PARTIALLY_FILLED
                               : OrderStatus::NEW;
                rest(std::move(order));
            } else {
                if (callbacks_.on_cancel) {
                    callbacks_.on_cancel(CancelEvent{
                        order.id, symbol_, order.side, order.price,
                        CancelReason::MARKET_NO_LIQUIDITY,
                        order.quantity_remaining, order.timestamp, sequence_
                    });
                }
            }
            break;

        case OrderType::FILL_OR_KILL:
            // FOK feasibility should have been checked by OrderValidators
            // before add() was called. If we still have quantity here it
            // means the pre-check was bypassed — cancel the whole thing.
            // All fills produced during match() are already emitted; the
            // FOK guarantee is enforced by the validator, not by undoing fills.
            // (Implementer note: enforce FOK atomically via pre-check, not post-cancel.)
            if (callbacks_.on_cancel) {
                callbacks_.on_cancel(CancelEvent{
                    order.id, symbol_, order.side, order.price,
                    CancelReason::FOK_FAILED,
                    order.quantity_remaining, order.timestamp, sequence_
                });
            }
            break;

        case OrderType::STOP:
        case OrderType::STOP_LIMIT:
            // Must not be routed to add() directly — route through
            // StopOrderManager. Fail loudly so the bug is obvious.
            throw std::logic_error(
                "TreeOrderBook::add() received a STOP or STOP_LIMIT order. "
                "Route stop orders through StopOrderManager instead.");

        default:
            throw std::logic_error("TreeOrderBook::add(): unknown order type");
        }
    } else {
        // Fully filled during match — no remainder handling needed.
    }

    // ── 4. Emit book snapshot ─────────────────────────────────────────────────
    emit_snapshot(order.timestamp);
}

// ─────────────────────────────────────────────────────────────────────────────
// cancel()
// ─────────────────────────────────────────────────────────────────────────────

void TreeOrderBook::cancel(uint64_t order_id) {
    auto map_it = order_map_.find(order_id);
    if (map_it == order_map_.end()) {
        return; // Already filled or unknown — no-op per contract.
    }

    const OrderLocation& loc = map_it->second;

    // Find the price level and erase the order from it in O(1).
    if (loc.side == Side::BID) {
        auto level_it = bids_.find(loc.price);
        if (level_it != bids_.end()) {
            uint64_t remaining = loc.it->quantity_remaining;
            level_it->second.erase(loc.it);
            if (level_it->second.empty()) {
                bids_.erase(level_it);
            }
            if (callbacks_.on_cancel) {
                callbacks_.on_cancel(CancelEvent{
                    order_id, symbol_, Side::BID, loc.price,
                    CancelReason::CLIENT_REQUEST,
                    remaining, 0, sequence_
                });
            }
        }
    } else {
        auto level_it = asks_.find(loc.price);
        if (level_it != asks_.end()) {
            uint64_t remaining = loc.it->quantity_remaining;
            level_it->second.erase(loc.it);
            if (level_it->second.empty()) {
                asks_.erase(level_it);
            }
            if (callbacks_.on_cancel) {
                callbacks_.on_cancel(CancelEvent{
                    order_id, symbol_, Side::ASK, loc.price,
                    CancelReason::CLIENT_REQUEST,
                    remaining, 0, sequence_
                });
            }
        }
    }

    order_map_.erase(map_it);
    emit_snapshot(0);
}

// ─────────────────────────────────────────────────────────────────────────────
// execute()
//
// Called when an external feed reports a fill against a resting order
// we're tracking (e.g. ITCH Execute Order message).
// ─────────────────────────────────────────────────────────────────────────────

void TreeOrderBook::execute(uint64_t order_id, uint64_t qty, uint64_t timestamp) {
    auto map_it = order_map_.find(order_id);
    if (map_it == order_map_.end()) {
        return; // Unknown order — no-op.
    }

    const OrderLocation& loc = map_it->second;

    // Fetch the order's current quantity before modifying.
    uint64_t fill_qty = std::min(qty, loc.it->quantity_remaining);

    if (callbacks_.on_fill) {
        callbacks_.on_fill(FillEvent{
            0,           // no aggressive order ID — feed-driven fill
            order_id,
            symbol_,
            opposite(loc.side), // aggressor is on the opposite side
            loc.price,
            fill_qty,
            timestamp,
            ++sequence_
        });
    }

    last_trade_price_ = loc.price;

    if (fill_qty >= loc.it->quantity_remaining) {
        // Fully consumed — remove from level and order_map.
        if (loc.side == Side::BID) {
            auto level_it = bids_.find(loc.price);
            if (level_it != bids_.end()) {
                level_it->second.erase(loc.it);
                if (level_it->second.empty()) bids_.erase(level_it);
            }
        } else {
            auto level_it = asks_.find(loc.price);
            if (level_it != asks_.end()) {
                level_it->second.erase(loc.it);
                if (level_it->second.empty()) asks_.erase(level_it);
            }
        }
        order_map_.erase(map_it);
    } else {
        // Partial feed fill — reduce quantity in place. Note: uses the
        // iterator-based reduce(), not reduce_front() — order_id may not be
        // at the front of this level's FIFO queue.
        if (loc.side == Side::BID) {
            auto level_it = bids_.find(loc.price);
            if (level_it != bids_.end()) {
                level_it->second.reduce(loc.it, fill_qty);
            }
        } else {
            auto level_it = asks_.find(loc.price);
            if (level_it != asks_.end()) {
                level_it->second.reduce(loc.it, fill_qty);
            }
        }
    }

    emit_snapshot(timestamp);
}

// ─────────────────────────────────────────────────────────────────────────────
// reduce()
//
// Feed-driven partial size reduction WITHOUT a trade (e.g. ITCH Order Cancel
// message). Distinct from execute(): fires on_cancel instead of on_fill, and
// the order keeps its place in time priority if it isn't fully consumed.
// ─────────────────────────────────────────────────────────────────────────────

void TreeOrderBook::reduce(uint64_t order_id, uint64_t qty, uint64_t timestamp) {
    auto map_it = order_map_.find(order_id);
    if (map_it == order_map_.end()) {
        return; // Unknown order — no-op.
    }

    const OrderLocation& loc = map_it->second;

    uint64_t reduce_qty      = std::min(qty, loc.it->quantity_remaining);
    uint64_t remaining_after = loc.it->quantity_remaining - reduce_qty;

    if (callbacks_.on_cancel) {
        callbacks_.on_cancel(CancelEvent{
            order_id, symbol_, loc.side, loc.price,
            CancelReason::CLIENT_REQUEST,
            remaining_after,
            timestamp,
            ++sequence_
        });
    }

    if (remaining_after == 0) {
        // Fully consumed — remove from level and order_map.
        if (loc.side == Side::BID) {
            auto level_it = bids_.find(loc.price);
            if (level_it != bids_.end()) {
                level_it->second.erase(loc.it);
                if (level_it->second.empty()) bids_.erase(level_it);
            }
        } else {
            auto level_it = asks_.find(loc.price);
            if (level_it != asks_.end()) {
                level_it->second.erase(loc.it);
                if (level_it->second.empty()) asks_.erase(level_it);
            }
        }
        order_map_.erase(map_it);
    } else {
        if (loc.side == Side::BID) {
            auto level_it = bids_.find(loc.price);
            if (level_it != bids_.end()) {
                level_it->second.reduce(loc.it, reduce_qty);
            }
        } else {
            auto level_it = asks_.find(loc.price);
            if (level_it != asks_.end()) {
                level_it->second.reduce(loc.it, reduce_qty);
            }
        }
    }

    emit_snapshot(timestamp);
}

// ─────────────────────────────────────────────────────────────────────────────
// replace()
// ─────────────────────────────────────────────────────────────────────────────

void TreeOrderBook::replace(uint64_t old_order_id, Order new_order) {
    cancel(old_order_id);
    rest(std::move(new_order));
    emit_snapshot(new_order.timestamp);
}

// ─────────────────────────────────────────────────────────────────────────────
// match() — the core matching loop
//
// Sweeps the opposite side from best price inward, calling the
// MatchingAlgorithm at each price level. Stops when:
//   - aggressive.quantity reaches 0 (fully filled), OR
//   - the next price level doesn't cross (no more matching prices)
// ─────────────────────────────────────────────────────────────────────────────

void TreeOrderBook::match(Order& aggressive) {
    if (!matcher_) return;

    // Which side are we sweeping?
    // BID aggressive → sweep ASK side from lowest ask upward
    // ASK aggressive → sweep BID side from highest bid downward
    bool is_buy = (aggressive.side == Side::BID);

    while (aggressive.quantity_remaining > 0) {

        // ── Find best opposing level ──────────────────────────────────────────
        bool has_opposite = is_buy ? !asks_.empty() : !bids_.empty();
        if (!has_opposite) break;

        int64_t best_opposing_price = is_buy
            ? asks_.begin()->first
            : bids_.begin()->first;

        // ── Price cross check ─────────────────────────────────────────────────
        bool price_crosses = false;
        if (aggressive.type == OrderType::MARKET ||
            aggressive.type == OrderType::MARKET_LIMIT) {
            price_crosses = true; // market orders always cross
        } else {
            // LIMIT / IOC / FOK:
            // BID: order price >= best ask → crosses
            // ASK: order price <= best bid → crosses
            price_crosses = is_buy
                ? (aggressive.price >= best_opposing_price)
                : (aggressive.price <= best_opposing_price);
        }

        if (!price_crosses) break;

        // ── Delegate allocation to MatchingAlgorithm ─────────────────────────
        PriceLevelInterface& level = is_buy
            ? static_cast<PriceLevelInterface&>(asks_.begin()->second)
            : static_cast<PriceLevelInterface&>(bids_.begin()->second);

        LevelMatchResult result = matcher_->match_level(
            aggressive,
            aggressive.quantity_remaining,
            level,
            aggressive.timestamp
        );

        // ── Process fills ─────────────────────────────────────────────────────
        for (auto& fill : result.fills) {
            fill.symbol   = symbol_;
            fill.sequence = ++sequence_;

            // Remove the passive order from order_map_ if fully filled.
            // A passive order is fully filled when fill.fill_quantity equals
            // its original resting quantity — the matcher already popped it
            // from the level, so we just clean up the hash map.
            auto passive_it = order_map_.find(fill.passive_order_id);
            if (passive_it != order_map_.end()) {
                bool fully_consumed = false;
                for (auto consumed_id : result.consumed_passive_orders) {
                    if (consumed_id == fill.passive_order_id) {
                        fully_consumed = true;
                        break;
                    }
                }
                if (fully_consumed) {
                    order_map_.erase(passive_it);
                }
            }

            last_trade_price_ = fill.fill_price;

            if (callbacks_.on_fill) {
                callbacks_.on_fill(fill);
            }
        }

        aggressive.quantity_remaining -= result.quantity_filled;

        // ── Prune empty price level ───────────────────────────────────────────
        if (result.level_exhausted) {
            if (is_buy) {
                asks_.erase(asks_.begin());
            } else {
                bids_.erase(bids_.begin());
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// rest() — place an order into the book as a resting (passive) order
// ─────────────────────────────────────────────────────────────────────────────

void TreeOrderBook::rest(Order order) {
    const int64_t  price = order.price;
    const Side     side  = order.side;
    const uint64_t id    = order.id;

    if (side == Side::BID) {
        // emplace returns pair<iterator, bool>; the iterator points to the
        // (possibly new) price level. We use try_emplace so existing levels
        // are not re-constructed.
        auto [level_it, _] = bids_.try_emplace(price, price);
        level_it->second.add(std::move(order));
        // Store the stable list iterator in order_map_ for O(1) cancel.
        order_map_[id] = {
            Side::BID,
            price,
            level_it->second.last_added_iterator()
        };
    } else {
        auto [level_it, _] = asks_.try_emplace(price, price);
        level_it->second.add(std::move(order));
        order_map_[id] = {
            Side::ASK,
            price,
            level_it->second.last_added_iterator()
        };
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// can_fill_fully() — FOK pre-check (read-only)
// ─────────────────────────────────────────────────────────────────────────────

bool TreeOrderBook::can_fill_fully(const Order& order) const noexcept {
    uint64_t needed    = order.quantity_remaining;
    bool     is_buy    = (order.side == Side::BID);

    if (is_buy) {
        for (auto& [ask_price, level] : asks_) {
            if (order.type != OrderType::MARKET && order.price < ask_price) break;
            if (needed <= level.total_qty()) return true;
            needed -= level.total_qty();
        }
    } else {
        for (auto& [bid_price, level] : bids_) {
            if (order.type != OrderType::MARKET && order.price > bid_price) break;
            if (needed <= level.total_qty()) return true;
            needed -= level.total_qty();
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// emit_snapshot()
// ─────────────────────────────────────────────────────────────────────────────

void TreeOrderBook::emit_snapshot(uint64_t timestamp) {
    if (!callbacks_.on_book_update) return;
    auto snap = snapshot(10);
    snap.timestamp = timestamp;
    snap.sequence  = ++sequence_;
    callbacks_.on_book_update(snap);
}

// ─────────────────────────────────────────────────────────────────────────────
// Query implementations
// ─────────────────────────────────────────────────────────────────────────────

std::optional<int64_t> TreeOrderBook::best_bid() const noexcept {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<int64_t> TreeOrderBook::best_ask() const noexcept {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::optional<int64_t> TreeOrderBook::spread() const noexcept {
    auto bid = best_bid();
    auto ask = best_ask();
    if (!bid || !ask) return std::nullopt;
    return *ask - *bid;
}

std::optional<double> TreeOrderBook::mid_price() const noexcept {
    auto bid = best_bid();
    auto ask = best_ask();
    if (!bid || !ask) return std::nullopt;
    return static_cast<double>(*bid + *ask) / 2.0;
}

std::optional<double> TreeOrderBook::weighted_mid() const noexcept {
    if (bids_.empty() || asks_.empty()) return std::nullopt;
    const auto& best_bid_level = bids_.begin()->second;
    const auto& best_ask_level = asks_.begin()->second;
    uint64_t bid_qty = best_bid_level.total_qty();
    uint64_t ask_qty = best_ask_level.total_qty();
    uint64_t total   = bid_qty + ask_qty;
    if (total == 0) return std::nullopt;
    // Weighted mid = (bid_price × ask_qty + ask_price × bid_qty) / total_qty
    // Weights are flipped: more ask qty → mid closer to bid (less buying pressure)
    double wmid = (static_cast<double>(bids_.begin()->first) * ask_qty +
                   static_cast<double>(asks_.begin()->first) * bid_qty)
                  / static_cast<double>(total);
    return wmid;
}

uint64_t TreeOrderBook::total_bid_qty() const noexcept {
    uint64_t total = 0;
    for (auto& [price, level] : bids_) total += level.total_qty();
    return total;
}

uint64_t TreeOrderBook::total_ask_qty() const noexcept {
    uint64_t total = 0;
    for (auto& [price, level] : asks_) total += level.total_qty();
    return total;
}

size_t TreeOrderBook::bid_depth() const noexcept {
    return bids_.size();
}

size_t TreeOrderBook::ask_depth() const noexcept {
    return asks_.size();
}

bool TreeOrderBook::has_order(uint64_t order_id) const noexcept {
    return order_map_.count(order_id) > 0;
}

int64_t TreeOrderBook::last_trade_price() const noexcept {
    return last_trade_price_;
}

const std::string& TreeOrderBook::symbol() const noexcept {
    return symbol_;
}

uint64_t TreeOrderBook::sequence() const noexcept {
    return sequence_;
}

BookSnapshot TreeOrderBook::snapshot(int depth) const {
    BookSnapshot snap;
    snap.symbol           = symbol_;
    snap.last_trade_price = last_trade_price_;
    snap.timestamp        = 0; // caller fills in if needed
    snap.sequence         = sequence_;

    int count = 0;
    for (auto& [price, level] : bids_) {
        if (depth >= 0 && count++ >= depth) break;
        snap.bids.push_back({price, level.total_qty(), level.order_count()});
    }
    count = 0;
    for (auto& [price, level] : asks_) {
        if (depth >= 0 && count++ >= depth) break;
        snap.asks.push_back({price, level.total_qty(), level.order_count()});
    }
    return snap;
}

} // namespace order_book