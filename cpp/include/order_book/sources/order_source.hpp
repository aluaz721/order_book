#pragma once

#include "../core/order.hpp"
#include "../core/event.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace order_book {

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
//   HistoricalReplayer    Reads a NASDAQ ITCH 5.0 binary or pre-parsed
//                         parquet file and feeds historical events in order.
//                         exhausted() returns true when the file is fully read.
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
//   - next_timestamp() returns the timestamp of the NEXT order this source
//     will produce. Return UINT64_MAX when exhausted or nothing pending.
//   - next_order() pops and returns that order. Must be O(1).
//   - Sources must not block. The engine runs on a single thread.
//   - Sources must not call back into the engine or the book.
// ─────────────────────────────────────────────────────────────────────────────

class OrderSource {
public:
    virtual ~OrderSource() = default;

    // Peek at the timestamp of the next order without consuming it.
    // Returns UINT64_MAX when exhausted or nothing is pending.
    // Must be O(1) and non-blocking.
    virtual uint64_t next_timestamp() const noexcept = 0;

    // Consume and return the next order.
    // Called by the engine only when this source has the minimum timestamp.
    // Returns std::nullopt if nothing is available at current_time (e.g.
    // InteractiveSource with an empty queue between user submissions).
    virtual std::optional<Order> next_order(uint64_t current_time) = 0;

    // True once the source will never produce another order.
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