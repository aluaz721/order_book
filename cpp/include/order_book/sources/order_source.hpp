#pragma once

#include "../core/order.hpp"
#include "../core/event.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// SourceEventType / SourceEvent
//
// A source doesn't only originate brand-new orders — historical feeds (ITCH,
// LOBSTER, etc.) also carry cancels, feed-driven executions, partial
// reductions, and replaces against orders the book already knows about.
// SourceEvent is the generic envelope every OrderSource emits so the engine
// can route each one to the right OrderBookInterface call without sources
// needing any direct access to the book (see the "sources must not call back
// into the book" contract below).
//
// Mapping to OrderBookInterface:
//   NEW_ORDER  →  add(order)                          (or StopOrderManager,
//                                                        for STOP/STOP_LIMIT)
//   CANCEL     →  cancel(target_order_id)              full removal
//   REDUCE     →  reduce(target_order_id, quantity, timestamp)
//                                                       partial size decrease
//                                                       WITHOUT a trade — the
//                                                       order keeps its place
//                                                       in the queue (e.g. an
//                                                       ITCH Order Cancel
//                                                       message)
//   EXECUTE    →  execute(target_order_id, quantity, timestamp)
//                                                       feed-driven fill (e.g.
//                                                       an ITCH Order Executed
//                                                       message) — a real
//                                                       trade against a
//                                                       counterparty the book
//                                                       isn't modelling
//   REPLACE    →  replace(target_order_id, order)       atomic cancel + add;
//                                                       the new order loses
//                                                       time priority
// ─────────────────────────────────────────────────────────────────────────────

enum class SourceEventType : uint8_t {
    NEW_ORDER,
    CANCEL,
    REDUCE,
    EXECUTE,
    REPLACE
};

struct SourceEvent {
    SourceEventType type;

    // Valid for NEW_ORDER and REPLACE (the new resting order).
    Order order{};

    // Valid for CANCEL, REDUCE, EXECUTE, REPLACE (the order being acted on;
    // for REPLACE this is the OLD order id being replaced).
    uint64_t target_order_id = 0;

    // Valid for REDUCE and EXECUTE — quantity to reduce / fill.
    uint64_t quantity = 0;

    // Simulation clock at which this event occurs. Always populated,
    // regardless of event type, so the engine has one field to read.
    uint64_t timestamp = 0;

    static SourceEvent new_order(Order o) noexcept {
        SourceEvent e;
        e.type      = SourceEventType::NEW_ORDER;
        e.timestamp = o.timestamp;
        e.order     = std::move(o);
        return e;
    }

    static SourceEvent cancel(uint64_t order_id, uint64_t timestamp) noexcept {
        SourceEvent e;
        e.type            = SourceEventType::CANCEL;
        e.target_order_id = order_id;
        e.timestamp       = timestamp;
        return e;
    }

    static SourceEvent reduce(uint64_t order_id, uint64_t quantity,
                              uint64_t timestamp) noexcept {
        SourceEvent e;
        e.type            = SourceEventType::REDUCE;
        e.target_order_id = order_id;
        e.quantity        = quantity;
        e.timestamp       = timestamp;
        return e;
    }

    static SourceEvent execute(uint64_t order_id, uint64_t quantity,
                               uint64_t timestamp) noexcept {
        SourceEvent e;
        e.type            = SourceEventType::EXECUTE;
        e.target_order_id = order_id;
        e.quantity        = quantity;
        e.timestamp       = timestamp;
        return e;
    }

    static SourceEvent replace(uint64_t old_order_id, Order new_order) noexcept {
        SourceEvent e;
        e.type            = SourceEventType::REPLACE;
        e.target_order_id = old_order_id;
        e.timestamp       = new_order.timestamp;
        e.order           = std::move(new_order);
        return e;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// OrderSource
//
// Abstract interface for any entity that submits orders to the
// SimulationEngine. The engine owns a priority queue of sources, calling
// next_order() on whichever source has the smallest next_timestamp() — giving
// a globally time-ordered merge of all sources regardless of their origin.
//
// Three concrete implementations:
//
//   HistoricalReplayer    Reads a NASDAQ ITCH 5.0 binary file and feeds
//                         historical events (adds, cancels, executes,
//                         replaces) in order. exhausted() returns true when
//                         the file is fully read.
//
//   StrategySource        A Python strategy callback invoked on each book
//                         update via pybind11. Buffers returned orders and
//                         feeds them to the engine on the next tick, preventing
//                         same-tick circular fills.
//
//   InteractiveSource     A thread-safe queue drained by the engine. The React
//                         UI submits orders from a FastAPI handler thread;
//                         the engine drains on its own thread. Never exhausted.
//
// Contract:
//   - next_timestamp() returns the timestamp of the NEXT event this source
//     will produce. Return UINT64_MAX when exhausted or nothing pending.
//   - next_order() pops and returns that event. Must be O(1).
//   - Sources must not block. The engine runs on a single thread.
//   - Sources must not call back into the engine or the book.
// ─────────────────────────────────────────────────────────────────────────────

class OrderSource {
public:
    virtual ~OrderSource() = default;

    // Peek at the timestamp of the next event without consuming it.
    // Returns UINT64_MAX when exhausted or nothing is pending.
    // Must be O(1) and non-blocking.
    virtual uint64_t next_timestamp() const noexcept = 0;

    // Consume and return the next event.
    // Called by the engine only when this source has the minimum timestamp.
    // Returns std::nullopt if nothing is available at current_time (e.g.
    // InteractiveSource with an empty queue between user submissions).
    virtual std::optional<SourceEvent> next_order(uint64_t current_time) = 0;

    // True once the source will never produce another event.
    // Used by the engine to prune exhausted sources from the priority queue.
    // HistoricalReplayer: true after EOF. StrategySource, InteractiveSource:
    // always false (live sources run until the engine is stopped).
    virtual bool exhausted() const noexcept = 0;

    // Human-readable identifier for logging, debugging, and benchmark labels.
    virtual const std::string& name() const noexcept = 0;

    // Called by the engine after each BookSnapshot is produced. Sources may
    // use this to react to book state changes (StrategySource uses it to
    // invoke the strategy callback). Default: no-op.
    virtual void on_book_update(const BookSnapshot& /*snapshot*/,
                                uint64_t            /*timestamp*/) {}
};

} // namespace order_book
