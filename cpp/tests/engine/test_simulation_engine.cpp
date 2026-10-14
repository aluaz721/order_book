#include <gtest/gtest.h>

#include "../../include/order_book/engine/simulation_engine.hpp"
#include "../../include/order_book/book/tree_order_book.hpp"
#include "../../include/order_book/matching/fifo_matcher.hpp"
#include "../../include/order_book/sources/interactive_source.hpp"
#include "../../include/order_book/sources/strategy_source.hpp"
#include "../../include/order_book/core/price_utils.hpp"

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// SimulationEngine tests
//
// The engine is the only route by which all seven order types are reachable:
// STOP and STOP_LIMIT are held by its StopOrderManager, and TreeOrderBook
// ::add() throws on them by design. These tests drive it the way the Python
// bindings and the API layer do — submit through an InteractiveSource, call
// run() to drain, inspect the resulting book and event stream.
// ─────────────────────────────────────────────────────────────────────────────

namespace order_book {

static Order make_order(uint64_t    id,
                        Side        side,
                        OrderType   type,
                        int64_t     price,
                        uint64_t    qty,
                        uint64_t    timestamp   = 0,
                        int64_t     limit_price = 0,
                        std::string symbol      = "AAPL")
{
    Order o{};
    o.id                 = id;
    o.symbol             = std::move(symbol);
    o.side               = side;
    o.type               = type;
    o.price              = price;
    o.limit_price        = limit_price;
    o.quantity_remaining = qty;
    o.orig_quantity      = qty;
    o.timestamp          = timestamp;
    o.status             = OrderStatus::NEW;
    return o;
}

class SimulationEngineTest : public ::testing::Test {
protected:
    std::vector<FillEvent>    fills_;
    std::vector<CancelEvent>  cancels_;
    std::vector<AckEvent>     acks_;
    std::vector<TriggerEvent> triggers_;
    std::vector<BookSnapshot> snapshots_;

    std::unique_ptr<SimulationEngine> engine_;
    InteractiveSource*                ui_ = nullptr;   // owned by engine_

    void SetUp() override {
        SimulationConfig config;
        config.symbol = "AAPL";
        // Converted stop orders draw IDs from this counter; keeping it clear
        // of the IDs the tests hand-assign makes correlation unambiguous.
        config.order_id_start = 100'000;
        build(config);
    }

    void build(SimulationConfig config) {
        engine_ = std::make_unique<SimulationEngine>(
            std::move(config),
            std::make_unique<TreeOrderBook>("AAPL", std::make_unique<FIFOMatcher>()));

        engine_->set_fill_callback    ([this](const FillEvent& e)    { fills_.push_back(e); });
        engine_->set_cancel_callback  ([this](const CancelEvent& e)  { cancels_.push_back(e); });
        engine_->set_ack_callback     ([this](const AckEvent& e)     { acks_.push_back(e); });
        engine_->set_trigger_callback ([this](const TriggerEvent& e) { triggers_.push_back(e); });
        engine_->set_snapshot_callback([this](const BookSnapshot& s) { snapshots_.push_back(s); });

        auto ui = std::make_unique<InteractiveSource>();
        ui_ = ui.get();
        engine_->add_source(std::move(ui));
    }

    // Submit and drain in one step — the engine returns from run() as soon as
    // its live sources are empty, which is exactly the API layer's usage.
    void submit(Order o) {
        ui_->submit(std::move(o));
        engine_->run();
    }

    void clear_events() {
        fills_.clear(); cancels_.clear(); acks_.clear();
        triggers_.clear(); snapshots_.clear();
    }

    // Two-sided market: bid 149.50, ask 150.50.
    void seed_market() {
        submit(make_order(1, Side::BID, OrderType::LIMIT, to_basis_points(149.50), 200));
        submit(make_order(2, Side::ASK, OrderType::LIMIT, to_basis_points(150.50), 200));
        clear_events();
    }

    const AckEvent* ack_for(uint64_t order_id) const {
        for (const auto& a : acks_) if (a.order_id == order_id) return &a;
        return nullptr;
    }
};

// ── Construction ─────────────────────────────────────────────────────────────

TEST_F(SimulationEngineTest, NullBookIsRejected) {
    EXPECT_THROW(
        SimulationEngine(SimulationConfig{}, nullptr),
        std::invalid_argument);
}

TEST_F(SimulationEngineTest, InitialStateIsClean) {
    EXPECT_EQ(engine_->orders_processed(), 0u);
    EXPECT_EQ(engine_->orders_rejected(), 0u);
    EXPECT_EQ(engine_->stops_triggered(), 0u);
    EXPECT_FALSE(engine_->is_running());
    EXPECT_EQ(engine_->stop_manager().pending_stop_count(), 0u);
}

TEST_F(SimulationEngineTest, TypedSourceAccessorsFindRegisteredSources) {
    EXPECT_EQ(engine_->interactive_source(), ui_);
    EXPECT_EQ(engine_->historical_replayer(), nullptr);
    EXPECT_EQ(engine_->strategy_source(), nullptr);

    engine_->add_source(std::make_unique<StrategySource>(
        [](const BookSnapshot&, uint64_t) { return std::vector<Order>{}; }));
    EXPECT_NE(engine_->strategy_source(), nullptr);
}

// ── All seven order types, end to end ────────────────────────────────────────

TEST_F(SimulationEngineTest, LimitOrderRestsInTheBook) {
    submit(make_order(1, Side::BID, OrderType::LIMIT, to_basis_points(150.00), 100));

    ASSERT_TRUE(engine_->book().best_bid().has_value());
    EXPECT_EQ(*engine_->book().best_bid(), to_basis_points(150.00));
    EXPECT_EQ(engine_->book().total_bid_qty(), 100u);
    EXPECT_EQ(engine_->orders_processed(), 1u);
}

TEST_F(SimulationEngineTest, MarketOrderFillsAndCancelsItsRemainder) {
    submit(make_order(1, Side::ASK, OrderType::LIMIT, to_basis_points(150.00), 40));
    clear_events();

    submit(make_order(2, Side::BID, OrderType::MARKET, 0, 100));

    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_quantity, 40u);
    ASSERT_EQ(cancels_.size(), 1u);
    EXPECT_EQ(cancels_[0].reason, CancelReason::MARKET_NO_LIQUIDITY);
    EXPECT_EQ(cancels_[0].remaining_quantity, 60u);
}

TEST_F(SimulationEngineTest, MarketLimitRestsItsRemainderAtTheLastTradedPrice) {
    submit(make_order(1, Side::ASK, OrderType::LIMIT, to_basis_points(150.00), 40));
    clear_events();

    // MARKET_LIMIT carries a price (validate_fields() requires a positive one
    // for every type but MARKET) which matching ignores — it always crosses,
    // like a MARKET order. The price submitted here is deliberately far from
    // the market to show the remainder rests at the traded price, not at it.
    submit(make_order(2, Side::BID, OrderType::MARKET_LIMIT,
                      to_basis_points(160.00), 100));

    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_quantity, 40u);
    EXPECT_EQ(fills_[0].fill_price, to_basis_points(150.00));
    // The 60 that couldn't fill rests as a bid at 150.00 — the last traded
    // price — rather than the 160.00 it was submitted with.
    ASSERT_TRUE(engine_->book().best_bid().has_value());
    EXPECT_EQ(*engine_->book().best_bid(), to_basis_points(150.00));
    EXPECT_EQ(engine_->book().total_bid_qty(), 60u);
}

TEST_F(SimulationEngineTest, MarketLimitWithoutAPriceIsRejected) {
    submit(make_order(1, Side::BID, OrderType::MARKET_LIMIT, /*price=*/0, 100));

    EXPECT_EQ(engine_->orders_rejected(), 1u);
    const AckEvent* ack = ack_for(1);
    ASSERT_NE(ack, nullptr);
    EXPECT_EQ(ack->status, OrderStatus::REJECTED);
}

TEST_F(SimulationEngineTest, IocFillsWhatItCanAndCancelsTheRest) {
    submit(make_order(1, Side::ASK, OrderType::LIMIT, to_basis_points(150.00), 40));
    clear_events();

    submit(make_order(2, Side::BID, OrderType::IMMEDIATE_OR_CANCEL,
                      to_basis_points(150.00), 100));

    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_quantity, 40u);
    ASSERT_EQ(cancels_.size(), 1u);
    EXPECT_EQ(cancels_[0].reason, CancelReason::IOC_EXPIRED);
    EXPECT_EQ(cancels_[0].remaining_quantity, 60u);
    EXPECT_EQ(engine_->book().total_bid_qty(), 0u);   // nothing rests
}

TEST_F(SimulationEngineTest, FokIsRejectedBeforeMatchingWhenItCannotFullyFill) {
    submit(make_order(1, Side::ASK, OrderType::LIMIT, to_basis_points(150.00), 40));
    clear_events();

    submit(make_order(2, Side::BID, OrderType::FILL_OR_KILL,
                      to_basis_points(150.00), 100));

    // The engine's validate_fok() pre-check rejects it, so the book never
    // sees it and the resting ask is untouched.
    EXPECT_TRUE(fills_.empty());
    EXPECT_EQ(engine_->orders_rejected(), 1u);
    const AckEvent* ack = ack_for(2);
    ASSERT_NE(ack, nullptr);
    EXPECT_EQ(ack->status, OrderStatus::REJECTED);
    EXPECT_NE(ack->message.find("shortfall"), std::string::npos);
    EXPECT_EQ(engine_->book().total_ask_qty(), 40u);
}

TEST_F(SimulationEngineTest, FokFillsCompletelyWhenLiquiditySuffices) {
    submit(make_order(1, Side::ASK, OrderType::LIMIT, to_basis_points(150.00), 60));
    submit(make_order(2, Side::ASK, OrderType::LIMIT, to_basis_points(150.25), 60));
    clear_events();

    submit(make_order(3, Side::BID, OrderType::FILL_OR_KILL,
                      to_basis_points(150.25), 100));

    uint64_t filled = 0;
    for (const auto& f : fills_) filled += f.fill_quantity;
    EXPECT_EQ(filled, 100u);
    EXPECT_EQ(engine_->orders_rejected(), 0u);
}

TEST_F(SimulationEngineTest, StopOrderIsHeldOutsideTheBookThenTriggersOnATrade) {
    seed_market();

    // Stop-sell at 149.00, below the 149.50 best bid.
    submit(make_order(10, Side::ASK, OrderType::STOP, to_basis_points(149.00), 50));

    const AckEvent* ack = ack_for(10);
    ASSERT_NE(ack, nullptr);
    EXPECT_EQ(ack->status, OrderStatus::PENDING_TRIGGER);
    EXPECT_EQ(engine_->stop_manager().pending_stop_count(), 1u);
    EXPECT_TRUE(engine_->stop_manager().has_stop(10));
    EXPECT_EQ(engine_->stop_manager().nearest_stop_sell(), to_basis_points(149.00));
    // Invisible in the book: only the seeded 200 bid / 200 ask.
    EXPECT_EQ(engine_->book().total_ask_qty(), 200u);

    // Another bid below the stop, so a big sell sweeps 149.50 then 148.50 —
    // the second fill is at-or-below 149.00 and fires the stop.
    submit(make_order(11, Side::BID, OrderType::LIMIT, to_basis_points(148.50), 200));
    clear_events();
    submit(make_order(12, Side::ASK, OrderType::LIMIT, to_basis_points(148.00), 250));

    ASSERT_EQ(triggers_.size(), 1u);
    EXPECT_EQ(triggers_[0].stop_order_id, 10u);
    EXPECT_EQ(triggers_[0].converted_to, OrderType::MARKET);
    EXPECT_GE(triggers_[0].triggered_order_id, 100'000u);  // from order_id_start
    EXPECT_EQ(engine_->stops_triggered(), 1u);
    EXPECT_EQ(engine_->stop_manager().pending_stop_count(), 0u);

    // The converted MARKET sell filled against the remaining 148.50 bid.
    bool filled_by_trigger = false;
    for (const auto& f : fills_) {
        if (f.aggressive_order_id == triggers_[0].triggered_order_id) {
            filled_by_trigger = true;
            EXPECT_EQ(f.fill_quantity, 50u);
        }
    }
    EXPECT_TRUE(filled_by_trigger);
}

TEST_F(SimulationEngineTest, StopLimitTriggersAsALimitOrderAtItsLimitPrice) {
    seed_market();

    // Sell-stop convention (order_validators.hpp): limit_price >= stop price,
    // so the order the stop converts into is passive rather than sweeping.
    submit(make_order(10, Side::ASK, OrderType::STOP_LIMIT, to_basis_points(149.00), 50,
                      /*timestamp=*/0, /*limit_price=*/to_basis_points(149.50)));
    EXPECT_EQ(engine_->orders_rejected(), 0u);
    EXPECT_EQ(engine_->stop_manager().pending_stop_count(), 1u);

    submit(make_order(11, Side::BID, OrderType::LIMIT, to_basis_points(148.50), 200));
    clear_events();
    submit(make_order(12, Side::ASK, OrderType::LIMIT, to_basis_points(148.00), 250));

    ASSERT_EQ(triggers_.size(), 1u);
    EXPECT_EQ(triggers_[0].converted_to, OrderType::LIMIT);

    // Converted to a LIMIT sell at 149.50, which does not cross the remaining
    // 148.50 bid — so unlike a STOP (which converts to MARKET and sweeps), it
    // rests. That difference is the whole point of STOP_LIMIT.
    for (const auto& f : fills_) {
        EXPECT_NE(f.aggressive_order_id, triggers_[0].triggered_order_id);
    }
    ASSERT_TRUE(engine_->book().best_ask().has_value());
    EXPECT_EQ(*engine_->book().best_ask(), to_basis_points(149.50));
    EXPECT_TRUE(engine_->book().has_order(triggers_[0].triggered_order_id));
}

TEST_F(SimulationEngineTest, StopBuyTriggersWhenThePriceRises) {
    seed_market();

    // Stop-buy at 151.00, above the 150.50 best ask.
    submit(make_order(10, Side::BID, OrderType::STOP, to_basis_points(151.00), 50));
    EXPECT_EQ(engine_->stop_manager().nearest_stop_buy(), to_basis_points(151.00));

    // Offer above the stop, then buy through it: fills at 150.50 (no trigger)
    // and 151.50 (triggers).
    submit(make_order(11, Side::ASK, OrderType::LIMIT, to_basis_points(151.50), 300));
    clear_events();
    submit(make_order(12, Side::BID, OrderType::LIMIT, to_basis_points(151.50), 250));

    ASSERT_EQ(triggers_.size(), 1u);
    EXPECT_EQ(triggers_[0].stop_order_id, 10u);
    EXPECT_EQ(triggers_[0].trigger_trade_price, to_basis_points(151.50));
    EXPECT_EQ(engine_->stops_triggered(), 1u);
}

// ── Stop validation (previously skipped entirely) ────────────────────────────

TEST_F(SimulationEngineTest, StopThatWouldTriggerImmediatelyIsRejected) {
    seed_market();

    // Stop-sell at 150.00 is ABOVE the 149.50 best bid — a trade at the bid
    // would fire it instantly, so it is not a stop at all.
    submit(make_order(10, Side::ASK, OrderType::STOP, to_basis_points(150.00), 50));

    EXPECT_EQ(engine_->orders_rejected(), 1u);
    const AckEvent* ack = ack_for(10);
    ASSERT_NE(ack, nullptr);
    EXPECT_EQ(ack->status, OrderStatus::REJECTED);
    // Rejected orders must not be parked in the stop manager.
    EXPECT_EQ(engine_->stop_manager().pending_stop_count(), 0u);
    EXPECT_FALSE(engine_->stop_manager().has_stop(10));
}

TEST_F(SimulationEngineTest, StopLimitWithLimitOnTheWrongSideOfTheStopIsRejected) {
    seed_market();

    // Buy stop convention: limit_price must be <= the stop price.
    submit(make_order(10, Side::BID, OrderType::STOP_LIMIT, to_basis_points(151.00), 50,
                      /*timestamp=*/0, /*limit_price=*/to_basis_points(152.00)));

    EXPECT_EQ(engine_->orders_rejected(), 1u);
    const AckEvent* ack = ack_for(10);
    ASSERT_NE(ack, nullptr);
    EXPECT_EQ(ack->status, OrderStatus::REJECTED);
    EXPECT_EQ(engine_->stop_manager().pending_stop_count(), 0u);
}

TEST_F(SimulationEngineTest, StopWithoutAPositiveStopPriceIsRejected) {
    seed_market();

    submit(make_order(10, Side::ASK, OrderType::STOP, /*price=*/0, 50));

    EXPECT_EQ(engine_->orders_rejected(), 1u);
    EXPECT_EQ(engine_->stop_manager().pending_stop_count(), 0u);
}

TEST_F(SimulationEngineTest, StopIsAcceptedOnAnEmptyBookWithNoReferencePrice) {
    // No bid, no ask, no last trade — validate_stop() has no reference price
    // to compare against and must not reject on that basis.
    submit(make_order(10, Side::ASK, OrderType::STOP, to_basis_points(149.00), 50));

    EXPECT_EQ(engine_->orders_rejected(), 0u);
    EXPECT_EQ(engine_->stop_manager().pending_stop_count(), 1u);
}

// ── Cancellation ─────────────────────────────────────────────────────────────

TEST_F(SimulationEngineTest, CancelThroughTheSourceRemovesARestingOrder) {
    submit(make_order(1, Side::BID, OrderType::LIMIT, to_basis_points(150.00), 100));
    clear_events();

    ui_->submit_cancel(1, "AAPL");
    engine_->run();

    EXPECT_FALSE(engine_->book().has_order(1));
    EXPECT_EQ(engine_->book().total_bid_qty(), 0u);
    ASSERT_EQ(cancels_.size(), 1u);
    EXPECT_EQ(cancels_[0].reason, CancelReason::CLIENT_REQUEST);
}

TEST_F(SimulationEngineTest, PendingStopCanBeCancelledThroughTheStopManager) {
    seed_market();
    submit(make_order(10, Side::ASK, OrderType::STOP, to_basis_points(149.00), 50));
    clear_events();

    engine_->stop_manager().cancel(10, engine_->current_time());

    EXPECT_EQ(engine_->stop_manager().pending_stop_count(), 0u);
    ASSERT_EQ(cancels_.size(), 1u);
    EXPECT_EQ(cancels_[0].reason, CancelReason::STOP_CANCELLED);

    // And it must not fire afterwards.
    submit(make_order(11, Side::BID, OrderType::LIMIT, to_basis_points(148.50), 200));
    submit(make_order(12, Side::ASK, OrderType::LIMIT, to_basis_points(148.00), 250));
    EXPECT_TRUE(triggers_.empty());
    EXPECT_EQ(engine_->stops_triggered(), 0u);
}

// ── Ordering, lifecycle, statistics ──────────────────────────────────────────

TEST_F(SimulationEngineTest, QueuedOrdersAreDrainedInSubmissionOrder) {
    // Three bids at the same price — FIFO priority follows submission order,
    // so an incoming sell must hit them in that order.
    ui_->submit(make_order(1, Side::BID, OrderType::LIMIT, to_basis_points(150.00), 10));
    ui_->submit(make_order(2, Side::BID, OrderType::LIMIT, to_basis_points(150.00), 10));
    ui_->submit(make_order(3, Side::BID, OrderType::LIMIT, to_basis_points(150.00), 10));
    engine_->run();
    EXPECT_EQ(engine_->orders_processed(), 3u);
    clear_events();

    submit(make_order(4, Side::ASK, OrderType::LIMIT, to_basis_points(150.00), 30));

    ASSERT_EQ(fills_.size(), 3u);
    EXPECT_EQ(fills_[0].passive_order_id, 1u);
    EXPECT_EQ(fills_[1].passive_order_id, 2u);
    EXPECT_EQ(fills_[2].passive_order_id, 3u);
}

TEST_F(SimulationEngineTest, RunReturnsWhenLiveSourcesAreDrainedAndIsResumable) {
    EXPECT_FALSE(engine_->is_running());
    engine_->run();                                  // nothing queued
    EXPECT_FALSE(engine_->is_running());
    EXPECT_EQ(engine_->orders_processed(), 0u);

    submit(make_order(1, Side::BID, OrderType::LIMIT, to_basis_points(150.00), 10));
    EXPECT_EQ(engine_->orders_processed(), 1u);

    submit(make_order(2, Side::BID, OrderType::LIMIT, to_basis_points(150.00), 10));
    EXPECT_EQ(engine_->orders_processed(), 2u);
    EXPECT_EQ(engine_->book().total_bid_qty(), 20u);
}

TEST_F(SimulationEngineTest, OrderIdIsAutoAssignedWhenLeftZero) {
    Order o = make_order(0, Side::BID, OrderType::LIMIT, to_basis_points(150.00), 10);
    submit(std::move(o));

    // InteractiveSource assigns a non-zero ID, so validate_fields() passes.
    EXPECT_EQ(engine_->orders_rejected(), 0u);
    EXPECT_EQ(engine_->book().total_bid_qty(), 10u);
    ASSERT_EQ(acks_.size(), 1u);
    EXPECT_NE(acks_[0].order_id, 0u);
}

TEST_F(SimulationEngineTest, SimulationClockFollowsEventTimestamps) {
    submit(make_order(1, Side::BID, OrderType::LIMIT, to_basis_points(150.00), 10,
                      /*timestamp=*/5'000));
    EXPECT_EQ(engine_->current_time(), 5'000u);

    submit(make_order(2, Side::BID, OrderType::LIMIT, to_basis_points(150.00), 10,
                      /*timestamp=*/9'000));
    EXPECT_EQ(engine_->current_time(), 9'000u);
}

TEST_F(SimulationEngineTest, EndTimeStopsTheEngine) {
    SimulationConfig config;
    config.symbol   = "AAPL";
    config.end_time = 1'000;
    build(config);

    ui_->submit(make_order(1, Side::BID, OrderType::LIMIT, to_basis_points(150.00), 10,
                           /*timestamp=*/5'000));
    engine_->run();

    // InteractiveSource reports timestamp 0 while it has pending work, which
    // is <= end_time, so the event is drained; the engine stops on the next
    // pass rather than mid-order.
    EXPECT_FALSE(engine_->is_running());
}

TEST_F(SimulationEngineTest, ResetClearsStatisticsButNotTheBook) {
    seed_market();
    submit(make_order(10, Side::ASK, OrderType::STOP, to_basis_points(150.00), 50)); // rejected
    EXPECT_GT(engine_->orders_processed(), 0u);
    EXPECT_EQ(engine_->orders_rejected(), 1u);

    engine_->reset();

    EXPECT_EQ(engine_->orders_processed(), 0u);
    EXPECT_EQ(engine_->orders_rejected(), 0u);
    EXPECT_EQ(engine_->stops_triggered(), 0u);
    EXPECT_EQ(engine_->tick_count(), 0u);
    EXPECT_EQ(engine_->current_time(), 0u);
    EXPECT_EQ(engine_->heap_size(), 0u);

    // reset() does not clear the book — OrderBookInterface has no clear().
    EXPECT_EQ(engine_->book().total_bid_qty(), 200u);

    // The engine is still usable afterwards.
    submit(make_order(20, Side::BID, OrderType::LIMIT, to_basis_points(149.00), 10));
    EXPECT_EQ(engine_->orders_processed(), 1u);
}

TEST_F(SimulationEngineTest, TickCallbackFiresOncePerEngineIteration) {
    std::vector<uint64_t> tick_times;
    engine_->set_tick_callback([&](uint64_t ts, const BookSnapshot&) {
        tick_times.push_back(ts);
    });

    ui_->submit(make_order(1, Side::BID, OrderType::LIMIT, to_basis_points(150.00), 10,
                           /*timestamp=*/7'000));
    engine_->run();

    ASSERT_FALSE(tick_times.empty());
    EXPECT_EQ(tick_times.front(), 7'000u);
    EXPECT_EQ(engine_->tick_count(), tick_times.size());
}

// ── StrategySource feedback loop ─────────────────────────────────────────────

TEST_F(SimulationEngineTest, StrategyOrdersAreDispatchedOnTheFollowingTick) {
    int invocations = 0;
    uint64_t strategy_order_id = 7'000;

    // Quote one bid a tick below the mid, but only once — a strategy that
    // emits on every book update would feed itself forever.
    engine_->add_source(std::make_unique<StrategySource>(
        [&](const BookSnapshot& snap, uint64_t ts) {
            invocations++;
            std::vector<Order> out;
            if (invocations == 1 && !snap.asks.empty()) {
                out.push_back(make_order(strategy_order_id, Side::BID, OrderType::LIMIT,
                                         snap.asks[0].price - to_basis_points(0.10),
                                         25, ts));
            }
            return out;
        }));

    submit(make_order(1, Side::ASK, OrderType::LIMIT, to_basis_points(150.00), 100));

    EXPECT_GE(invocations, 1);
    // The strategy's bid was buffered during the ask's book update and
    // dispatched on a later tick — by the time run() returns it is resting.
    EXPECT_TRUE(engine_->book().has_order(strategy_order_id));
    ASSERT_TRUE(engine_->book().best_bid().has_value());
    EXPECT_EQ(*engine_->book().best_bid(), to_basis_points(149.90));
    EXPECT_EQ(engine_->strategy_source()->total_orders_submitted(), 1u);
}

TEST_F(SimulationEngineTest, StrategyCanSubmitStopOrdersToo) {
    seed_market();

    int invocations = 0;
    engine_->add_source(std::make_unique<StrategySource>(
        [&](const BookSnapshot&, uint64_t ts) {
            invocations++;
            std::vector<Order> out;
            if (invocations == 1) {
                out.push_back(make_order(7'001, Side::ASK, OrderType::STOP,
                                         to_basis_points(149.00), 25, ts));
            }
            return out;
        }));

    submit(make_order(3, Side::BID, OrderType::LIMIT, to_basis_points(149.00), 10));

    // Routed to the stop manager, not the book.
    EXPECT_EQ(engine_->stop_manager().pending_stop_count(), 1u);
    EXPECT_TRUE(engine_->stop_manager().has_stop(7'001));
    EXPECT_FALSE(engine_->book().has_order(7'001));
}

// ── Source heap does not accumulate duplicates ───────────────────────────────

TEST_F(SimulationEngineTest, SourceHeapStaysBoundedWithTwoLiveSources) {
    // Two live sources (interactive + strategy) are the case that used to
    // grow the heap: the engine re-pushed every live source after every
    // dispatched event and relied on discarding the duplicates later.
    int invocations = 0;
    engine_->add_source(std::make_unique<StrategySource>(
        [&](const BookSnapshot&, uint64_t ts) {
            invocations++;
            std::vector<Order> out;
            if (invocations <= 40) {
                out.push_back(make_order(50'000 + invocations, Side::BID,
                                         OrderType::LIMIT,
                                         to_basis_points(100.00), 1, ts));
            }
            return out;
        }));

    size_t max_heap = 0;
    engine_->set_tick_callback([&](uint64_t, const BookSnapshot&) {
        max_heap = std::max(max_heap, engine_->heap_size());
    });

    for (uint64_t i = 1; i <= 200; ++i) {
        ui_->submit(make_order(i, Side::BID, OrderType::LIMIT,
                               to_basis_points(140.00), 1, i));
    }
    engine_->run();

    EXPECT_EQ(engine_->orders_processed(), 240u);  // 200 interactive + 40 strategy
    // At most one live entry per source plus a superseded entry awaiting
    // discard. Before the fix this grew roughly in step with the event count.
    EXPECT_LE(max_heap, 2 * 2u + 1u);
    EXPECT_LE(engine_->heap_size(), 2 * 2u + 1u);
}

} // namespace order_book
