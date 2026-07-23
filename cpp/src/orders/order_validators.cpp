#include "../../include/order_book/orders/order_validators.hpp"

namespace order_book {
namespace OrderValidators {

// ─────────────────────────────────────────────────────────────────────────────
// validate_fields()
// ─────────────────────────────────────────────────────────────────────────────

ValidationResult validate_fields(const Order& order) {
    if (order.id == 0) {
        return ValidationResult::fail("order id must be non-zero");
    }
    if (order.symbol.empty()) {
        return ValidationResult::fail("symbol must not be empty");
    }
    if (order.quantity_remaining == 0) {
        return ValidationResult::fail("quantity must be positive");
    }

    // MARKET orders ignore the price field entirely — every other type is
    // priced (IOC/FOK cross exactly like LIMIT — see TreeOrderBook::match()).
    if (order.type != OrderType::MARKET && order.price <= 0) {
        return ValidationResult::fail("price must be positive for " +
                                      std::string(to_string(order.type)) +
                                      " orders");
    }

    if (order.type == OrderType::STOP_LIMIT && order.limit_price <= 0) {
        return ValidationResult::fail("limit_price must be positive for STOP_LIMIT orders");
    }

    return ValidationResult::ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// validate_fok()
//
// Walks crossing price levels on the opposite side (via a full-depth
// snapshot) accumulating available quantity, exactly mirroring how the book
// itself would sweep levels during matching — but without mutating any
// state. Stops early once enough quantity has been found.
// ─────────────────────────────────────────────────────────────────────────────

ValidationResult validate_fok(const Order& order, const OrderBookInterface& book) {
    if (order.type != OrderType::FILL_OR_KILL) {
        return ValidationResult::ok();
    }

    const BookSnapshot snap = book.snapshot(-1); // all levels
    const auto& opposite_levels = (order.side == Side::BID) ? snap.asks : snap.bids;

    uint64_t available = 0;
    for (const auto& level : opposite_levels) {
        const bool crosses = (order.side == Side::BID)
                                  ? (level.price <= order.price)
                                  : (level.price >= order.price);
        if (!crosses) break; // levels are price-ordered — nothing further crosses

        available += level.quantity;
        if (available >= order.quantity_remaining) {
            return ValidationResult::ok();
        }
    }

    const uint64_t shortfall = order.quantity_remaining - available;
    return ValidationResult::fail(
        "FOK cannot be fully filled: shortfall of " + std::to_string(shortfall) +
        " shares (available=" + std::to_string(available) +
        ", requested=" + std::to_string(order.quantity_remaining) + ")");
}

// ─────────────────────────────────────────────────────────────────────────────
// validate_stop()
// ─────────────────────────────────────────────────────────────────────────────

ValidationResult validate_stop(const Order& order, const OrderBookInterface& book) {
    if (order.type != OrderType::STOP && order.type != OrderType::STOP_LIMIT) {
        return ValidationResult::ok();
    }

    if (order.side == Side::BID) {
        // Stop-buy: must trigger above the current market, or it would fire
        // immediately. Reference price is the best ask, falling back to the
        // last trade price if the book has no asks yet.
        const auto ref = book.best_ask();
        const int64_t reference = ref.has_value() ? *ref : book.last_trade_price();
        if (reference > 0 && order.price <= reference) {
            return ValidationResult::fail(
                "stop-buy price must be above the current market to avoid triggering immediately");
        }
    } else {
        // Stop-sell: must trigger below the current market.
        const auto ref = book.best_bid();
        const int64_t reference = ref.has_value() ? *ref : book.last_trade_price();
        if (reference > 0 && order.price >= reference) {
            return ValidationResult::fail(
                "stop-sell price must be below the current market to avoid triggering immediately");
        }
    }

    if (order.type == OrderType::STOP_LIMIT) {
        // Convention documented in order_validators.hpp: buy stops carry a
        // limit_price at or below the stop (trigger) price; sell stops carry
        // one at or above it.
        const bool ok = (order.side == Side::BID)
                            ? (order.limit_price <= order.price)
                            : (order.limit_price >= order.price);
        if (!ok) {
            return ValidationResult::fail(
                order.side == Side::BID
                    ? "STOP_LIMIT buy: limit_price must be <= stop price"
                    : "STOP_LIMIT sell: limit_price must be >= stop price");
        }
    }

    return ValidationResult::ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// validate() — composite entry point used by SimulationEngine::dispatch()
// ─────────────────────────────────────────────────────────────────────────────

ValidationResult validate(const Order& order, const OrderBookInterface& book) {
    ValidationResult result = validate_fields(order);
    if (!result.valid) return result;

    if (order.type == OrderType::FILL_OR_KILL) {
        result = validate_fok(order, book);
        if (!result.valid) return result;
    }

    if (order.type == OrderType::STOP || order.type == OrderType::STOP_LIMIT) {
        result = validate_stop(order, book);
        if (!result.valid) return result;
    }

    return ValidationResult::ok();
}

} // namespace OrderValidators
} // namespace order_book
