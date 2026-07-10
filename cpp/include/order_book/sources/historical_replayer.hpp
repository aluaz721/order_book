#pragma once

#include "order_source.hpp"
#include "../core/order.hpp"
#include <string>
#include <optional>
#include <cstdint>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// ReplaySpeed
//
// Controls the pacing of historical event replay.
//
//   FAST       No sleep between events. Replay as fast as the CPU allows.
//              Used for backtesting — you want results immediately.
//
//   REALTIME   Sleep between events to maintain wall-clock timing.
//              The engine uses the inter-event timestamp delta to sleep,
//              preserving the original pace of the recorded session.
//              Used for UI demos that should look like live trading.
//
//   FIXED_NS   Fixed inter-event delay in nanoseconds (configurable).
//              Useful for demo videos or regression tests that need
//              deterministic event pacing regardless of timestamp gaps.
// ─────────────────────────────────────────────────────────────────────────────

enum class ReplaySpeed {
    FAST,
    REALTIME,
    FIXED_NS
};

// ─────────────────────────────────────────────────────────────────────────────
// HistoricalReplayer (post-MVP)
//
// Reads a NASDAQ ITCH 5.0 binary file and feeds historical order book events
// to the SimulationEngine in timestamp order.
//
// ITCH 5.0 message types handled:
//   'A' — Add Order (no MPID)
//   'F' — Add Order with MPID attribution
//   'E' — Order Executed
//   'C' — Order Executed with Price (non-displayed liquidity)
//   'X' — Order Cancel (partial)
//   'D' — Order Delete
//   'U' — Order Replace (cancel + add at new price/qty)
//
// Message types silently skipped (not order events):
//   'S' — System Event (market open/close signals — available via callback)
//   'H' — Stock Trading Action (halts)
//   'Y' — Reg SHO short sale restriction
//   'L', 'V', 'W', 'K', 'J', 'h' — various market-wide events
//   'I' — NOII (Net Order Imbalance Indicator)
//   'N' — Retail Price Improvement Indicator
//
// Symbol filtering:
//   If symbol_filter is non-empty, only events matching that symbol are
//   replayed. Events for other symbols are parsed but discarded. This allows
//   efficient single-symbol simulation from a multi-symbol ITCH feed without
//   pre-filtering the file.
//
// Lookahead buffer:
//   The replayer always has the next parsed order in a lookahead buffer so
//   next_timestamp() can return the upcoming timestamp without consuming the
//   order. This is required by the SimulationEngine's priority queue merge.
//
// NOTE: Post-MVP. Declare the header now for forward compatibility; the
//       implementation (.cpp) is built under AQUILA_BUILD_HISTORICAL_REPLAY.
// ─────────────────────────────────────────────────────────────────────────────

class HistoricalReplayer : public OrderSource {
public:

    struct Config {
        std::string  path;            // path to ITCH .bin or .gz file
        std::string  symbol_filter;   // empty = replay all symbols
        ReplaySpeed  speed      = ReplaySpeed::FAST;
        uint64_t     fixed_ns   = 0;  // inter-event delay for FIXED_NS mode
        uint64_t     start_time = 0;  // skip events before this ns timestamp
        uint64_t     end_time   = 0;  // stop after this ns timestamp (0 = no limit)
        bool         verbose    = false; // log parsed message counts to stderr
    };

    explicit HistoricalReplayer(Config config);

    // ── OrderSource interface ─────────────────────────────────────────────────

    uint64_t             next_timestamp() const noexcept override;
    std::optional<Order> next_order(uint64_t current_time) override;
    bool                 exhausted()      const noexcept override;
    const std::string&   name()           const noexcept override { return name_; }

    void on_book_update(const BookSnapshot& snapshot,
                        uint64_t            timestamp) override;

    // ── Statistics ────────────────────────────────────────────────────────────

    uint64_t messages_parsed()    const noexcept { return messages_parsed_; }
    uint64_t orders_replayed()    const noexcept { return orders_replayed_; }
    uint64_t orders_skipped()     const noexcept { return orders_skipped_; }
    uint64_t bytes_read()         const noexcept { return bytes_read_; }

private:
    void advance();   // parse the next ITCH message into lookahead_

    Config              config_;
    std::string         name_ = "HistoricalReplayer";

    // Opaque file state — implementation detail (FILE* or mmap handle)
    // Declared as void* here to avoid platform-specific includes in the header.
    void*               file_handle_    = nullptr;

    std::optional<Order> lookahead_;     // next order ready to be returned
    bool                 exhausted_     = false;

    uint64_t messages_parsed_  = 0;
    uint64_t orders_replayed_  = 0;
    uint64_t orders_skipped_   = 0;
    uint64_t bytes_read_       = 0;
};

} // namespace order_book