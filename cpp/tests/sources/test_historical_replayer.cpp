#include <gtest/gtest.h>

#include "../../include/order_book/book/tree_order_book.hpp"
#include "../../include/order_book/matching/fifo_matcher.hpp"
#include "../../include/order_book/engine/simulation_engine.hpp"
#include "../../include/order_book/sources/historical_replayer.hpp"

#include <fstream>
#include <string>
#include <vector>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// A small, hand-built ITCH 5.0 byte encoder, independent of the production
// parser in historical_replayer.cpp, so these tests actually exercise the
// wire format rather than just checking the parser agrees with itself.
//
// Framing: [2-byte big-endian length][message bytes], matching what
// HistoricalReplayer expects (NASDAQ's downloadable ITCH 5.0 sample format).
// ─────────────────────────────────────────────────────────────────────────────

namespace {

void put_u16(std::string& buf, uint16_t v) {
    buf.push_back(static_cast<char>((v >> 8) & 0xFF));
    buf.push_back(static_cast<char>(v & 0xFF));
}

void put_u32(std::string& buf, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        buf.push_back(static_cast<char>((v >> shift) & 0xFF));
    }
}

void put_u48(std::string& buf, uint64_t v) {
    for (int shift = 40; shift >= 0; shift -= 8) {
        buf.push_back(static_cast<char>((v >> shift) & 0xFF));
    }
}

void put_u64(std::string& buf, uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        buf.push_back(static_cast<char>((v >> shift) & 0xFF));
    }
}

void put_stock(std::string& buf, const std::string& symbol) {
    std::string padded = symbol;
    padded.resize(8, ' ');
    buf += padded;
}

void append_framed(std::string& file, const std::string& payload) {
    put_u16(file, static_cast<uint16_t>(payload.size()));
    file += payload;
}

std::string build_add(uint64_t order_ref, char side, uint32_t shares,
                      const std::string& stock, uint32_t price_bp,
                      uint64_t timestamp_ns, uint16_t stock_locate = 1) {
    std::string m;
    m.push_back('A');
    put_u16(m, stock_locate);
    put_u16(m, 0); // tracking number
    put_u48(m, timestamp_ns);
    put_u64(m, order_ref);
    m.push_back(side);
    put_u32(m, shares);
    put_stock(m, stock);
    put_u32(m, price_bp);
    return m;
}

std::string build_execute(uint64_t order_ref, uint32_t executed_shares,
                          uint64_t timestamp_ns, uint16_t stock_locate = 1) {
    std::string m;
    m.push_back('E');
    put_u16(m, stock_locate);
    put_u16(m, 0);
    put_u48(m, timestamp_ns);
    put_u64(m, order_ref);
    put_u32(m, executed_shares);
    put_u64(m, 0); // match number
    return m;
}

std::string build_cancel(uint64_t order_ref, uint32_t cancelled_shares,
                         uint64_t timestamp_ns, uint16_t stock_locate = 1) {
    std::string m;
    m.push_back('X');
    put_u16(m, stock_locate);
    put_u16(m, 0);
    put_u48(m, timestamp_ns);
    put_u64(m, order_ref);
    put_u32(m, cancelled_shares);
    return m;
}

std::string build_delete(uint64_t order_ref, uint64_t timestamp_ns,
                         uint16_t stock_locate = 1) {
    std::string m;
    m.push_back('D');
    put_u16(m, stock_locate);
    put_u16(m, 0);
    put_u48(m, timestamp_ns);
    put_u64(m, order_ref);
    return m;
}

std::string build_replace(uint64_t old_ref, uint64_t new_ref, uint32_t shares,
                          uint32_t price_bp, uint64_t timestamp_ns,
                          uint16_t stock_locate = 1) {
    std::string m;
    m.push_back('U');
    put_u16(m, stock_locate);
    put_u16(m, 0);
    put_u48(m, timestamp_ns);
    put_u64(m, old_ref);
    put_u64(m, new_ref);
    put_u32(m, shares);
    put_u32(m, price_bp);
    return m;
}

// Writes `content` to a fresh temp file and returns its path.
std::string write_temp_itch_file(const std::string& content, const std::string& tag) {
    std::string path = ::testing::TempDir() + "itch_test_" + tag + ".bin";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    return path;
}

} // namespace

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// Main scenario: Add / Execute / Cancel(partial) / Delete / Replace, replayed
// end-to-end through SimulationEngine into a TreeOrderBook.
// ─────────────────────────────────────────────────────────────────────────────

TEST(HistoricalReplayerTest, ReplaysFullOrderLifecycleCorrectly) {
    std::string file;

    // 1. Resting bid: 100 @ 150.0000, id=1001
    append_framed(file, build_add(1001, 'B', 100, "AAPL", 1500000, 1000));
    // 2. Resting ask: 50 @ 150.0500, id=1002 (no cross — different price)
    append_framed(file, build_add(1002, 'S', 50, "AAPL", 1500500, 2000));
    // 3. Resting bid: 30 @ 149.9900, id=1003 (will be replaced later)
    append_framed(file, build_add(1003, 'B', 30, "AAPL", 1499900, 3000));
    // 4. Feed-driven fill: 40 shares executed against id=1001 (100 -> 60)
    append_framed(file, build_execute(1001, 40, 4000));
    // 5. Partial cancel (no trade): 20 shares off id=1001 (60 -> 40)
    append_framed(file, build_cancel(1001, 20, 5000));
    // 6. Full delete of the resting ask, id=1002
    append_framed(file, build_delete(1002, 6000));
    // 7. Replace id=1003 -> id=2003, new size 25 @ 149.9800
    append_framed(file, build_replace(1003, 2003, 25, 1499800, 7000));

    const std::string path = write_temp_itch_file(file, "lifecycle");

    std::vector<FillEvent>   fills;
    std::vector<CancelEvent> cancels;

    SimulationConfig sim_config;
    sim_config.symbol = "AAPL";

    SimulationEngine engine(
        sim_config,
        std::make_unique<TreeOrderBook>("AAPL", std::make_unique<FIFOMatcher>()));

    engine.set_fill_callback([&](const FillEvent& e)     { fills.push_back(e); });
    engine.set_cancel_callback([&](const CancelEvent& e) { cancels.push_back(e); });

    HistoricalReplayer::Config replay_config;
    replay_config.path = path;
    engine.add_source(std::make_unique<HistoricalReplayer>(replay_config));

    engine.run();

    // ── Replay accounting ──────────────────────────────────────────────────
    ASSERT_NE(engine.historical_replayer(), nullptr);
    EXPECT_EQ(engine.historical_replayer()->messages_parsed(), 7u);
    EXPECT_EQ(engine.historical_replayer()->orders_replayed(), 7u);
    EXPECT_EQ(engine.historical_replayer()->orders_skipped(), 0u);
    EXPECT_TRUE(engine.historical_replayer()->exhausted());
    EXPECT_EQ(engine.orders_processed(), 7u);

    // ── Fills: exactly one, from the Executed message ──────────────────────
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].aggressive_order_id, 0u); // feed-driven, no aggressor order
    EXPECT_EQ(fills[0].passive_order_id, 1001u);
    EXPECT_EQ(fills[0].fill_price, 1500000);
    EXPECT_EQ(fills[0].fill_quantity, 40u);
    EXPECT_EQ(fills[0].aggressor_side, Side::ASK); // opposite of the resting BID

    // ── Cancels: partial reduce (1001), full delete (1002), and the
    //    replace-induced cancel of the old order (1003) ─────────────────────
    ASSERT_EQ(cancels.size(), 3u);
    EXPECT_EQ(cancels[0].order_id, 1001u);
    EXPECT_EQ(cancels[0].remaining_quantity, 40u); // 100 - 40 (exec) - 20 (cancel)
    EXPECT_EQ(cancels[1].order_id, 1002u);
    EXPECT_EQ(cancels[1].remaining_quantity, 50u); // full size, never touched
    EXPECT_EQ(cancels[2].order_id, 1003u);         // replace() cancels the old id

    // ── Final book state ─────────────────────────────────────────────────
    const auto& book = engine.book();

    EXPECT_TRUE(book.has_order(1001));
    EXPECT_FALSE(book.has_order(1002)); // deleted
    EXPECT_FALSE(book.has_order(1003)); // replaced away
    EXPECT_TRUE(book.has_order(2003));  // the replacement

    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(*book.best_bid(), 1500000); // 1001 still resting at 150.00
    EXPECT_FALSE(book.best_ask().has_value()); // 1002 was the only ask, deleted

    EXPECT_EQ(book.total_bid_qty(), 65u); // 40 (id 1001) + 25 (id 2003)
    EXPECT_EQ(book.bid_depth(), 2u);      // 150.0000 and 149.9800
    EXPECT_EQ(book.ask_depth(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Symbol filtering: only Add messages matching symbol_filter are tracked;
// every subsequent message referencing a filtered-out order id must also be
// dropped (since it's simply absent from the replayer's tracking map).
// ─────────────────────────────────────────────────────────────────────────────

TEST(HistoricalReplayerTest, SymbolFilterDropsOtherSymbolsEntirely) {
    std::string file;
    append_framed(file, build_add(1, 'B', 100, "AAPL", 1500000, 1000));
    append_framed(file, build_add(2, 'S', 50,  "MSFT", 3000000, 2000));
    append_framed(file, build_execute(2, 10, 3000)); // references the filtered MSFT order

    const std::string path = write_temp_itch_file(file, "symbol_filter");

    SimulationConfig sim_config;
    sim_config.symbol = "AAPL";

    SimulationEngine engine(
        sim_config,
        std::make_unique<TreeOrderBook>("AAPL", std::make_unique<FIFOMatcher>()));

    HistoricalReplayer::Config replay_config;
    replay_config.path          = path;
    replay_config.symbol_filter = "AAPL";
    engine.add_source(std::make_unique<HistoricalReplayer>(replay_config));

    engine.run();

    EXPECT_EQ(engine.historical_replayer()->messages_parsed(), 3u);
    EXPECT_EQ(engine.historical_replayer()->orders_replayed(), 1u); // only the AAPL add
    EXPECT_EQ(engine.historical_replayer()->orders_skipped(), 2u); // MSFT add + its execute

    EXPECT_TRUE(engine.book().has_order(1));
    EXPECT_FALSE(engine.book().has_order(2));
}

// ─────────────────────────────────────────────────────────────────────────────
// start_time / end_time window
// ─────────────────────────────────────────────────────────────────────────────

TEST(HistoricalReplayerTest, StartAndEndTimeWindowAreRespected) {
    std::string file;
    append_framed(file, build_add(1, 'B', 10, "AAPL", 1000000, 1000)); // before window
    append_framed(file, build_add(2, 'B', 20, "AAPL", 1000000, 5000)); // in window
    append_framed(file, build_add(3, 'B', 30, "AAPL", 1000000, 9000)); // after window

    const std::string path = write_temp_itch_file(file, "time_window");

    SimulationConfig sim_config;
    sim_config.symbol = "AAPL";

    SimulationEngine engine(
        sim_config,
        std::make_unique<TreeOrderBook>("AAPL", std::make_unique<FIFOMatcher>()));

    HistoricalReplayer::Config replay_config;
    replay_config.path       = path;
    replay_config.start_time = 4000;
    replay_config.end_time   = 6000;
    engine.add_source(std::make_unique<HistoricalReplayer>(replay_config));

    engine.run();

    EXPECT_FALSE(engine.book().has_order(1)); // before start_time
    EXPECT_TRUE(engine.book().has_order(2));  // inside the window
    EXPECT_FALSE(engine.book().has_order(3)); // past end_time — replayer stopped
    EXPECT_TRUE(engine.historical_replayer()->exhausted());
}

// ─────────────────────────────────────────────────────────────────────────────
// Non-order message types ('S' System Event here) must be skipped silently
// without affecting message accounting for real order events.
// ─────────────────────────────────────────────────────────────────────────────

TEST(HistoricalReplayerTest, NonOrderMessageTypesAreSkippedSilently) {
    std::string file;

    // 'S' System Event: type(1) + stock locate(2) + tracking(2) + ts(6) + event code(1) = 12 bytes
    std::string system_event;
    system_event.push_back('S');
    put_u16(system_event, 1);
    put_u16(system_event, 0);
    put_u48(system_event, 500);
    system_event.push_back('O'); // "Start of Messages" event code
    append_framed(file, system_event);

    append_framed(file, build_add(1, 'B', 10, "AAPL", 1000000, 1000));

    const std::string path = write_temp_itch_file(file, "system_event");

    SimulationConfig sim_config;
    sim_config.symbol = "AAPL";

    SimulationEngine engine(
        sim_config,
        std::make_unique<TreeOrderBook>("AAPL", std::make_unique<FIFOMatcher>()));

    HistoricalReplayer::Config replay_config;
    replay_config.path = path;
    engine.add_source(std::make_unique<HistoricalReplayer>(replay_config));

    engine.run();

    EXPECT_EQ(engine.historical_replayer()->messages_parsed(), 2u); // S + A
    EXPECT_EQ(engine.historical_replayer()->orders_replayed(), 1u); // only the Add
    EXPECT_EQ(engine.historical_replayer()->orders_skipped(), 0u);  // S isn't "skipped", it's not an order event
    EXPECT_TRUE(engine.book().has_order(1));
}

} // namespace order_book
