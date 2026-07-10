#pragma once

#include "../core/order.hpp"
#include "../book/order_book_interface.hpp"
#include <string>
#include <optional>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// ValidationResult
//
// Returned by all validator functions. On failure, `reason` contains a
// human-readable explanation emitted as the AckEvent message and logged.
// ─────────────────────────────────────────────────────────────────────────────

struct ValidationResult {
    bool        valid;
    std::string reason;  // empty if valid == true

    static ValidationResult ok()                       { return {true, {}}; }
    static ValidationResult fail(std::string reason)   { return {false, std::move(reason)}; }
};

// ─────────────────────────────────────────────────────────────────────────────
// OrderValidators
//
// Stateless validation functions called by the SimulationEngine before
// routing an order to the OrderBook or StopOrderManager. Validation
// checks structural correctness and basic feasibility only — it does not
// interact with book state (except for FOK feasibility, which needs the book
// to count available liquidity).
//
// Validation is intentionally separate from matching logic so that:
//   1. Tests can validate orders independently of a live book.
//   2. New order types can add validators without touching matching code.
//   3. The risk manager (Python layer) can pre-validate before calling C++.
//
// Validators are applied in order:
//   1. validate_fields()    — structural checks, no book access
//   2. validate_fok()       — FOK feasibility check (needs book)
//   3. validate_stop()      — stop price / limit price relationship
// ─────────────────────────────────────────────────────────────────────────────

namespace OrderValidators {

// ── Field validation (no book access) ────────────────────────────────────────

// Check structural correctness of any order type:
//   - id != 0
//   - symbol non-empty
//   - quantity > 0
//   - price > 0 for LIMIT, STOP, STOP_LIMIT, MARKET_LIMIT
//   - limit_price > 0 for STOP_LIMIT
//   - MARKET orders: price field is ignored (not validated)
ValidationResult validate_fields(const Order& order);

// ── FOK feasibility (requires book access) ────────────────────────────────────

// Pre-check whether a FILL_OR_KILL order can be fully filled without
// modifying book state. Counts available quantity at crossing price levels.
//
// Returns ok() if the FOK can be fully filled; fail() with the shortfall
// quantity in the reason string if not.
//
// Only meaningful for OrderType::FILL_OR_KILL. Returns ok() for all others.
//
// This must be called before any matching occurs, and must be a pure read
// of the book — it must not modify any state.
ValidationResult validate_fok(const Order& order,
                               const OrderBookInterface& book);

// ── Stop order validation (no book access) ────────────────────────────────────

// For STOP and STOP_LIMIT orders:
//   - BID stop  (stop-buy):  stop_price must be ABOVE the current best ask
//                             (otherwise it would trigger immediately)
//   - ASK stop  (stop-sell): stop_price must be BELOW the current best bid
//   - STOP_LIMIT additionally: limit_price relationship to stop_price is
//                               validated based on side (limit_price <=
//                               stop_price for buy stops, >= for sell stops
//                               is a common but configurable convention)
//
// `last_trade_price` is used as a fallback if no best bid/ask is available.
ValidationResult validate_stop(const Order&             order,
                                const OrderBookInterface& book);

// ── Composite: run all applicable validators ──────────────────────────────────

// Calls validate_fields(), then type-appropriate validators in order.
// Returns the first failure encountered, or ok() if all pass.
// This is the function called by SimulationEngine for each incoming order.
ValidationResult validate(const Order&             order,
                           const OrderBookInterface& book);

} // namespace OrderValidators

} // namespace order_book