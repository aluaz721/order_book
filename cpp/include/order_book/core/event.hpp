#pragma once

#include "order.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// FillEvent
//
// Emitted once per match between an aggressive (incoming) order and a passive
// (resting) order. A single aggressive order may produce multiple FillEvents
// if it sweeps through several resting orders or price levels.
//
// fill_price is always the passive order's resting price (the maker's price),
// consistent with price-time priority matching semantics.
// ─────────────────────────────────────────────────────────────────────────────

struct FillEvent {
    uint64_t    aggressive_order_id;
    uint64_t    passive_order_id;
    std::string symbol;
    Side        aggressor_side;
    int64_t     fill_price;           // basis points — passive order's price
    uint64_t    fill_quantity;        // quantity matched in this event
    uint64_t    timestamp;            // nanoseconds since epoch
    uint64_t    sequence;             // monotonically increasing per-book counter
};

// ─────────────────────────────────────────────────────────────────────────────
// CancelEvent
// ─────────────────────────────────────────────────────────────────────────────

enum class CancelReason : uint8_t {
    CLIENT_REQUEST,    // explicit cancel submitted by an OrderSource
    IOC_EXPIRED,       // IOC: unfilled remainder after immediate match attempt
    FOK_FAILED,        // FOK: insufficient liquidity for a full fill
    MARKET_NO_LIQUIDITY, // MARKET order couldn't fully fill; remainder cancelled
    RISK_REJECTED,     // blocked before matching by order validator
    STOP_CANCELLED,    // a pending stop/stop-limit was explicitly cancelled
    SELF_TRADE         // would have matched own order (if STP enabled)
};

// returns a string representation of the cancel reason
inline const char* to_string(CancelReason r) noexcept {
    switch (r) {
        case CancelReason::CLIENT_REQUEST:      return "CLIENT_REQUEST";
        case CancelReason::IOC_EXPIRED:         return "IOC_EXPIRED";
        case CancelReason::FOK_FAILED:          return "FOK_FAILED";
        case CancelReason::MARKET_NO_LIQUIDITY: return "MARKET_NO_LIQUIDITY";
        case CancelReason::RISK_REJECTED:       return "RISK_REJECTED";
        case CancelReason::STOP_CANCELLED:      return "STOP_CANCELLED";
        case CancelReason::SELF_TRADE:          return "SELF_TRADE";
        default:                                return "UNKNOWN";
    }
}

struct CancelEvent {
    uint64_t     order_id;
    std::string  symbol;
    Side         side;
    int64_t      price;               // basis points
    CancelReason reason;
    uint64_t     remaining_quantity;  // unfilled qty at time of cancel
    uint64_t     timestamp;
    uint64_t     sequence;
};

// ─────────────────────────────────────────────────────────────────────────────
// TriggerEvent
//
// Emitted by StopOrderManager when a stop or stop-limit order is triggered
// and converted into a live market or limit order.
// ─────────────────────────────────────────────────────────────────────────────

struct TriggerEvent {
    uint64_t    stop_order_id;        // the original stop order
    uint64_t    triggered_order_id;   // the new live order injected into the book
    std::string symbol;
    Side        side;
    int64_t     stop_price;           // the trigger price that was crossed
    int64_t     trigger_trade_price;  // the actual trade price that caused the trigger
    OrderType   converted_to;         // MARKET or LIMIT
    uint64_t    timestamp;
};

// ─────────────────────────────────────────────────────────────────────────────
// AckEvent
//
// Emitted once per submitted order to confirm acceptance into the book
// (or rejection). Consumed by the simulation engine and interactive UI.
// ─────────────────────────────────────────────────────────────────────────────

struct AckEvent {
    uint64_t     order_id;
    std::string  symbol;
    OrderStatus  status;   // NEW or PENDING_TRIGGER or REJECTED
    std::string  message;  // human-readable rejection reason if REJECTED
    uint64_t     timestamp;
};

// ─────────────────────────────────────────────────────────────────────────────
// BookLevel / BookSnapshot
//
// Represents the visible state of the order book at a point in time.
// Sent to the Python feature engine and the WebSocket stream on every
// state change. `sequence` allows consumers to detect missed events.
// ─────────────────────────────────────────────────────────────────────────────

struct BookLevel {
    int64_t  price;          // basis points
    uint64_t quantity;       // total resting quantity at this level
    uint32_t order_count;    // number of individual orders at this level
};

struct BookSnapshot {
    std::string            symbol;
    std::vector<BookLevel> bids;      // sorted best (highest price) → worst
    std::vector<BookLevel> asks;      // sorted best (lowest price)  → worst
    int64_t                last_trade_price;  // 0 if no trades yet
    uint64_t               timestamp;
    uint64_t               sequence;
};

// ─────────────────────────────────────────────────────────────────────────────
// Callback typedefs
//
// Defined here so they can be shared across OrderBook, StopOrderManager,
// and SimulationEngine without circular includes.
// ─────────────────────────────────────────────────────────────────────────────

using FillCallback     = std::function<void(const FillEvent&)>;
using CancelCallback   = std::function<void(const CancelEvent&)>;
using TriggerCallback  = std::function<void(const TriggerEvent&)>;
using AckCallback      = std::function<void(const AckEvent&)>;
using SnapshotCallback = std::function<void(const BookSnapshot&)>;

} // namespace order_book