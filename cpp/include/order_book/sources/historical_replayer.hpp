#pragma once

#include "order_source.hpp"
#include "../core/order.hpp"
#include <string>
#include <optional>
#include <unordered_map>
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
// HistoricalReplayer
//
// Reads a NASDAQ ITCH 5.0 binary file and feeds historical order book events
// to the SimulationEngine in timestamp order.
//
// File framing:
//   Each message is expected to be framed as [2-byte big-endian length]
//   [message bytes], repeated until EOF. This is the framing used by NASDAQ's
//   publicly downloadable ITCH 5.0 sample files (e.g. from emi.nasdaq.com) —
//   NOT the MoldUDP64 multicast session wrapper used for the live feed, which
//   has its own sequence/session header and would need to be de-multiplexed
//   into individual messages before being handed to this reader.
//
// Timestamps:
//   ITCH message timestamps are nanoseconds SINCE MIDNIGHT of the trading
//   day the file covers — ITCH carries no date. Every Order.timestamp and
//   SourceEvent.timestamp this replayer produces is that same raw
//   nanoseconds-since-midnight value, NOT Unix epoch time. Config::start_time
//   / end_time are compared against that same clock. Callers that need
//   wall-clock timestamps must add the trading day's midnight offset
//   themselves.
//
// ITCH 5.0 message types handled:
//   'A' — Add Order (no MPID)             → SourceEvent::new_order
//   'F' — Add Order with MPID attribution  → SourceEvent::new_order
//   'E' — Order Executed                   → SourceEvent::execute
//   'C' — Order Executed with Price        → SourceEvent::execute (the
//                                             non-displayed execution price
//                                             is parsed but not surfaced —
//                                             OrderBookInterface::execute()
//                                             always fills at the resting
//                                             order's own price)
//   'X' — Order Cancel (partial)           → SourceEvent::reduce (keeps
//                                             time priority — no trade)
//   'D' — Order Delete                     → SourceEvent::cancel
//   'U' — Order Replace (cancel + add)     → SourceEvent::replace
//
// Message types silently skipped (not order events):
//   'S' — System Event (market open/close signals)
//   'H' — Stock Trading Action (halts)
//   'Y' — Reg SHO short sale restriction
//   'L', 'V', 'W', 'K', 'J', 'h' — various market-wide events
//   'I' — NOII (Net Order Imbalance Indicator)
//   'N' — Retail Price Improvement Indicator
//   'R' — Stock Directory, 'P'/'Q' — trade/cross messages, and any other
//         type not listed above.
//
// Order reference tracking:
//   ITCH messages other than Add carry only an 8-byte order reference number
//   — no symbol, side, or price. The replayer keeps a map of order reference
//   number → {symbol, side, remaining quantity} for every order it has
//   emitted, populated on Add and consulted (and updated / erased) on every
//   subsequent Executed/Cancel/Delete/Replace so those events can be
//   reconstructed and, when a symbol_filter is set, correctly attributed
//   without needing to resolve NASDAQ's compact `stock_locate` code (which
//   requires the Stock Directory message this replayer doesn't parse). Map
//   entries are erased once an order is fully consumed or deleted, so memory
//   stays bounded to currently-resting orders rather than growing with the
//   whole file.
//
// Symbol filtering:
//   If symbol_filter is non-empty, only Add messages for that symbol are
//   tracked (and therefore replayed); all other messages are parsed (to stay
//   correctly positioned in the file) but discarded. Because non-Add message
//   types are matched purely via the order reference number map, an
//   Executed/Cancel/Delete/Replace for a filtered-out symbol is automatically
//   dropped too — it simply won't be found in the map.
//
// Lookahead buffer:
//   The replayer always has the next emitted event in a lookahead buffer so
//   next_timestamp() can return the upcoming timestamp without consuming it.
//   This is required by the SimulationEngine's priority queue merge.
// ─────────────────────────────────────────────────────────────────────────────

class HistoricalReplayer : public OrderSource {
public:

    struct Config {
        std::string  path;            // path to an ITCH 5.0 binary file
        std::string  symbol_filter;   // empty = replay all symbols
        ReplaySpeed  speed      = ReplaySpeed::FAST;
        uint64_t     fixed_ns   = 0;  // inter-event delay for FIXED_NS mode
        uint64_t     start_time = 0;  // skip events before this ns-since-midnight
        uint64_t     end_time   = 0;  // stop once this ns-since-midnight is exceeded (0 = no limit)
        bool         verbose    = false; // log parsed message counts to stderr
    };

    explicit HistoricalReplayer(Config config);
    ~HistoricalReplayer() override;

    // Owns a file handle — not copyable. Not moved either (OrderSource
    // instances are always held via std::unique_ptr by the engine).
    HistoricalReplayer(const HistoricalReplayer&)            = delete;
    HistoricalReplayer& operator=(const HistoricalReplayer&) = delete;

    // ── OrderSource interface ─────────────────────────────────────────────────

    uint64_t                    next_timestamp() const noexcept override;
    std::optional<SourceEvent>  next_order(uint64_t current_time) override;
    bool                        exhausted()      const noexcept override;
    const std::string&          name()           const noexcept override { return name_; }

    void on_book_update(const BookSnapshot& snapshot,
                        uint64_t            timestamp) override;

    // ── Statistics ────────────────────────────────────────────────────────────

    uint64_t messages_parsed()    const noexcept { return messages_parsed_; }
    uint64_t orders_replayed()    const noexcept { return orders_replayed_; }
    uint64_t orders_skipped()     const noexcept { return orders_skipped_; }
    uint64_t bytes_read()         const noexcept { return bytes_read_; }

private:
    // Order reference number → what we need to remember about a resting
    // order once its Add message has scrolled out of the lookahead buffer.
    struct TrackedOrder {
        std::string symbol;
        Side        side;
        uint64_t    remaining_qty;
    };

    void advance();   // parse ITCH messages into lookahead_ until one survives
                       // filtering or EOF is reached

    Config              config_;
    std::string         name_ = "HistoricalReplayer";

    // Opaque file state (a std::ifstream*) — declared as void* here to avoid
    // a <fstream> include (and its transitive cost) in this header.
    void*               file_handle_    = nullptr;

    std::unordered_map<uint64_t, TrackedOrder> active_orders_;

    std::optional<SourceEvent> lookahead_;   // next event ready to be returned
    bool                       exhausted_   = false;

    uint64_t messages_parsed_  = 0;
    uint64_t orders_replayed_  = 0;
    uint64_t orders_skipped_   = 0;
    uint64_t bytes_read_       = 0;
};

} // namespace order_book
