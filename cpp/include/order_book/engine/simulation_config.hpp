#pragma once

#include <cstdint>
#include <string>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// SimulationConfig
//
// Configuration struct for SimulationEngine. All fields have sane defaults
// so callers only need to set what they care about.
// ─────────────────────────────────────────────────────────────────────────────

struct SimulationConfig {

    // ── Instrument ────────────────────────────────────────────────────────────

    std::string symbol = "UNKNOWN";

    // ── Snapshot / streaming ──────────────────────────────────────────────────

    // Number of price levels included in each BookSnapshot sent to Python /
    // the WebSocket stream. Deeper snapshots give more feature richness at
    // the cost of serialization overhead. 10 is a sensible default.
    int snapshot_depth = 10;

    // If true, emit a BookSnapshot after every single order event (maximum
    // granularity). If false, emit once per engine tick (once per source's
    // batch of orders at a given timestamp). Use true for interactive/UI
    // simulation; false for fast batch backtesting where snapshot volume
    // would dominate runtime.
    bool emit_on_every_order = true;

    // ── Time bounds ───────────────────────────────────────────────────────────

    // Simulation clock start/end in nanoseconds since Unix epoch.
    // Events outside [start_time, end_time] are skipped.
    // end_time == 0 means "run until all sources are exhausted or stop() called".
    uint64_t start_time = 0;
    uint64_t end_time   = 0;

    // ── Performance ───────────────────────────────────────────────────────────

    // Maximum number of orders to process before yielding to a stop() check.
    // Relevant only when the engine is run in a thread alongside a UI.
    // 0 = no yield (process until exhaustion or stop()).
    uint64_t yield_every_n_orders = 0;

    // ── Order ID generation ───────────────────────────────────────────────────

    // Starting value for the engine's internal order ID counter.
    // InteractiveSource and StopOrderManager share this counter.
    // Set to a large value to avoid collisions with IDs from historical feeds
    // (ITCH order IDs are typically small integers starting from 1).
    uint64_t order_id_start = 1'000'000'000ULL;

    // ── Diagnostics ───────────────────────────────────────────────────────────

    // Log order processing statistics to stderr on engine completion.
    bool verbose = false;
};

} // namespace order_book