#pragma once

#include <cstdint>
#include <string>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// Side
// ─────────────────────────────────────────────────────────────────────────────

enum class Side : uint8_t {
    BID,  // buy side
    ASK   // sell side
};

// returns the opposite side (BID <-> ASK)
inline Side opposite(Side s) noexcept {
    return s == Side::BID ? Side::ASK : Side::BID;
}

// returns a string representation of the side
inline const char* to_string(Side s) noexcept {
    return s == Side::BID ? "BID" : "ASK";
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderType
//
// Supported types:
//
//   LIMIT          Rest in the book at the specified price if not immediately
//                  matchable.
//
//   MARKET         Match at any available price. Never rests. If insufficient
//                  liquidity exists to fully fill, the remainder is cancelled.
//
//   MARKET_LIMIT   Behaves as MARKET but, if it cannot be immediately filled
//                  in full, the unfilled remainder rests at the last traded
//                  price (i.e. it converts to a LIMIT order at that price).
//                  Used to guarantee execution without chasing the market.
//
//   STOP           Passive trigger. Rests in the StopOrderManager (not in the
//                  visible book). When the last trade price crosses the stop
//                  price, converts to a MARKET order and is injected into the
//                  book. Stop buys trigger when price rises through stop price;
//                  stop sells when price falls through stop price.
//
//   STOP_LIMIT     Like STOP, but converts to a LIMIT order at `limit_price`
//                  rather than a MARKET order when triggered. The limit price
//                  must be set separately from the stop (trigger) price.
//
//   IMMEDIATE_OR_CANCEL (IOC)
//                  Match as much as possible immediately, cancel any unfilled
//                  remainder. Never rests.
//
//   FILL_OR_KILL (FOK)
//                  Must be fully filled immediately or the entire order is
//                  cancelled. Feasibility is checked before any matching
//                  occurs — no partial fills are ever produced.
// ─────────────────────────────────────────────────────────────────────────────

enum class OrderType : uint8_t {
    LIMIT,
    MARKET,
    MARKET_LIMIT,
    STOP,
    STOP_LIMIT,
    IMMEDIATE_OR_CANCEL,
    FILL_OR_KILL
};

// returns a string representation of the order type
inline const char* to_string(OrderType t) noexcept {
    switch (t) {
        case OrderType::LIMIT:               return "LIMIT";
        case OrderType::MARKET:              return "MARKET";
        case OrderType::MARKET_LIMIT:        return "MARKET_LIMIT";
        case OrderType::STOP:                return "STOP";
        case OrderType::STOP_LIMIT:          return "STOP_LIMIT";
        case OrderType::IMMEDIATE_OR_CANCEL: return "IOC";
        case OrderType::FILL_OR_KILL:        return "FOK";
        default:                             return "UNKNOWN";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderStatus
// ─────────────────────────────────────────────────────────────────────────────

enum class OrderStatus : uint8_t {
    NEW,               // accepted, awaiting processing
    PENDING_TRIGGER,   // STOP or STOP_LIMIT waiting for trigger condition
    PARTIALLY_FILLED,  // some quantity filled, remainder resting or cancelled
    FILLED,            // fully executed
    CANCELLED,         // cancelled (IOC remainder, FOK failure, explicit cancel)
    REJECTED           // rejected before entering the book (validation failure)
};

// returns a string representation of the order status
inline const char* to_string(OrderStatus s) noexcept {
    switch (s) {
        case OrderStatus::NEW:              return "NEW";
        case OrderStatus::PENDING_TRIGGER:  return "PENDING_TRIGGER";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::FILLED:           return "FILLED";
        case OrderStatus::CANCELLED:        return "CANCELLED";
        case OrderStatus::REJECTED:         return "REJECTED";
        default:                            return "UNKNOWN";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Order
//
// Plain data struct — no methods, no virtual dispatch. All prices are stored
// as int64_t in basis points (hundredths of a cent) to eliminate floating-
// point rounding errors throughout the matching engine.
//
// Price encoding:
//   $150.05  →  1500500  (multiply by 10,000)
//   Use price_utils.hpp helpers for conversion.
//
// Stop/stop-limit orders use both fields:
//   price        →  stop (trigger) price
//   limit_price  →  the limit price the order converts to on trigger
//                   (ignored for plain STOP orders)
//
// Time-in-force semantics are encoded in OrderType:
//   LIMIT / MARKET_LIMIT / STOP / STOP_LIMIT  →  GTC (good till cancelled)
//   IMMEDIATE_OR_CANCEL                        →  IOC
//   FILL_OR_KILL                               →  FOK
//   MARKET                                     →  implicit IOC (auto-cancelled
//                                                  if insufficient liquidity)
// ─────────────────────────────────────────────────────────────────────────────

struct Order {
    uint64_t    id;                       // unique identifier assigned by the source
    std::string symbol;                   // instrument identifier, e.g. "AAPL"
    Side        side;
    OrderType   type;
    int64_t     price;                    // basis points; trigger price for STOP/STOP_LIMIT
    int64_t     limit_price;              // basis points; used only by STOP_LIMIT
    uint64_t    quantity_remaining;       // remaining unfilled quantity (mutated during matching)
    uint64_t    orig_quantity;            // original submitted quantity (never mutated)
    uint64_t    timestamp;                // nanoseconds since Unix epoch
    OrderStatus status;
};

} // namespace order_book