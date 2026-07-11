#include "../../include/order_book/orders/stop_order_manager.hpp"

#include <cassert>
#include <utility>
#include <algorithm>

namespace order_book {

StopOrderManager::StopOrderManager(OrderBookInterface&    book,
                                     TriggerCallback        on_trigger,
                                     CancelCallback         on_cancel,
                                     std::function<uint64_t()> next_order_id)
    : book_(book)
    , on_trigger_(std::move(on_trigger))
    , on_cancel_(std::move(on_cancel))
    , next_order_id_(std::move(next_order_id))
{}

void StopOrderManager::submit(Order order, AckCallback on_ack) {
    const bool accepted_type = (order.type == OrderType::STOP) ||
                               (order.type == OrderType::STOP_LIMIT);

    if (!accepted_type) {
        assert(false && "StopOrderManager::submit() only accepts STOP and STOP_LIMIT orders.");
        if (on_ack) {
            on_ack(AckEvent{
                order.id,
                order.symbol,
                OrderStatus::REJECTED,
                "StopOrderManager only accepts STOP and STOP_LIMIT orders",
                order.timestamp
            });
        }
        return;
    }

    if (order.id == 0 || order.symbol.empty() || order.quantity_remaining == 0) {
        if (on_ack) {
            on_ack(AckEvent{
                order.id,
                order.symbol,
                OrderStatus::REJECTED,
                "Invalid stop order fields",
                order.timestamp
            });
        }
        return;
    }

    if (order.type == OrderType::STOP_LIMIT && order.limit_price <= 0) {
        if (on_ack) {
            on_ack(AckEvent{
                order.id,
                order.symbol,
                OrderStatus::REJECTED,
                "STOP_LIMIT order requires a positive limit_price",
                order.timestamp
            });
        }
        return;
    }

    if (order.price <= 0) {
        if (on_ack) {
            on_ack(AckEvent{
                order.id,
                order.symbol,
                OrderStatus::REJECTED,
                "Stop price must be positive",
                order.timestamp
            });
        }
        return;
    }

    order.status = OrderStatus::PENDING_TRIGGER;

    if (order.side == Side::BID) {
        stop_buys_[order.price].push_back(order);
        stop_map_[order.id] = StopLocation{order.side, order.price};
    } else {
        stop_sells_[order.price].push_back(order);
        stop_map_[order.id] = StopLocation{order.side, order.price};
    }

    if (on_ack) {
        on_ack(AckEvent{
            order.id,
            order.symbol,
            OrderStatus::PENDING_TRIGGER,
            {},
            order.timestamp
        });
    }
}

void StopOrderManager::cancel(uint64_t order_id, uint64_t timestamp) {
    auto it = stop_map_.find(order_id);
    if (it == stop_map_.end()) {
        return;
    }

    const StopLocation& location = it->second;

    if (location.side == Side::BID) {
        auto price_it = stop_buys_.find(location.price);
        if (price_it != stop_buys_.end()) {
            StopQueue& queue = price_it->second;
            auto queue_it = std::find_if(queue.begin(), queue.end(),
                                         [order_id](const Order& stop_order) {
                                             return stop_order.id == order_id;
                                         });
            if (queue_it != queue.end()) {
                const uint64_t remaining_qty = queue_it->quantity_remaining;
                queue.erase(queue_it);
                if (queue.empty()) {
                    stop_buys_.erase(price_it);
                }
                if (on_cancel_) {
                    on_cancel_(CancelEvent{
                        order_id,
                        book_.symbol(),
                        Side::BID,
                        location.price,
                        CancelReason::STOP_CANCELLED,
                        remaining_qty,
                        timestamp,
                        book_.sequence()
                    });
                }
            }
        }
    } else {
        auto price_it = stop_sells_.find(location.price);
        if (price_it != stop_sells_.end()) {
            StopQueue& queue = price_it->second;
            auto queue_it = std::find_if(queue.begin(), queue.end(),
                                         [order_id](const Order& stop_order) {
                                             return stop_order.id == order_id;
                                         });
            if (queue_it != queue.end()) {
                const uint64_t remaining_qty = queue_it->quantity_remaining;
                queue.erase(queue_it);
                if (queue.empty()) {
                    stop_sells_.erase(price_it);
                }
                if (on_cancel_) {
                    on_cancel_(CancelEvent{
                        order_id,
                        book_.symbol(),
                        Side::ASK,
                        location.price,
                        CancelReason::STOP_CANCELLED,
                        remaining_qty,
                        timestamp,
                        book_.sequence()
                    });
                }
            }
        }
    }

    stop_map_.erase(order_id);
}

void StopOrderManager::on_trade(int64_t trade_price, uint64_t timestamp) {
    // Phase 1: collect and remove from maps atomically
    std::vector<Order> to_trigger;

    auto buy_it = stop_buys_.begin();
    while (buy_it != stop_buys_.end() && buy_it->first <= trade_price) {
        for (auto& order : buy_it->second) {
            stop_map_.erase(order.id);
            to_trigger.push_back(std::move(order));
        }
        buy_it = stop_buys_.erase(buy_it);
    }

    auto sell_it = stop_sells_.begin();
    while (sell_it != stop_sells_.end() && sell_it->first >= trade_price) {
        for (auto& order : sell_it->second) {
            stop_map_.erase(order.id);
            to_trigger.push_back(std::move(order));
        }
        sell_it = stop_sells_.erase(sell_it);
    }

    // Phase 2: submit — re-entrant on_trade() calls are now safe because
    // the maps are already clean
    for (auto& order : to_trigger) {
        trigger(order, trade_price, timestamp);
    }
}

size_t StopOrderManager::pending_stop_count() const noexcept {
    size_t count = 0;
    for (const auto& [_, queue] : stop_buys_) {
        count += queue.size();
    }
    for (const auto& [_, queue] : stop_sells_) {
        count += queue.size();
    }
    return count;
}

bool StopOrderManager::has_stop(uint64_t order_id) const noexcept {
    return stop_map_.find(order_id) != stop_map_.end();
}

int64_t StopOrderManager::nearest_stop_buy() const noexcept {
    return stop_buys_.empty() ? 0 : stop_buys_.begin()->first;
}

int64_t StopOrderManager::nearest_stop_sell() const noexcept {
    return stop_sells_.empty() ? 0 : stop_sells_.begin()->first;
}

void StopOrderManager::trigger(Order& stop_order, int64_t trade_price, uint64_t timestamp) {
    auto stop_it = stop_map_.find(stop_order.id);
    if (stop_it == stop_map_.end()) {
        return;
    }

    const StopLocation& location = stop_it->second;
    Order live_order;
    live_order.id = next_order_id_();
    live_order.symbol = stop_order.symbol;
    live_order.side = stop_order.side;
    live_order.type = (stop_order.type == OrderType::STOP)
        ? OrderType::MARKET
        : OrderType::LIMIT;
    live_order.price = (live_order.type == OrderType::LIMIT)
        ? stop_order.limit_price
        : 0;
    live_order.limit_price = (live_order.type == OrderType::LIMIT)
        ? stop_order.limit_price
        : 0;
    live_order.quantity_remaining = stop_order.quantity_remaining;
    live_order.orig_quantity = stop_order.orig_quantity;
    live_order.timestamp = timestamp;
    live_order.status = OrderStatus::NEW;

    if (on_trigger_) {
        on_trigger_(TriggerEvent{
            stop_order.id,
            live_order.id,
            stop_order.symbol,
            stop_order.side,
            stop_order.price,
            trade_price,
            live_order.type,
            timestamp
        });
    }

    book_.add(std::move(live_order));
}

void StopOrderManager::trigger_queue(StopQueue& queue, int64_t trade_price, uint64_t timestamp) {
    for (Order& stop_order : queue) {
        trigger(stop_order, trade_price, timestamp);
    }
    queue.clear();
}

} // namespace order_book
