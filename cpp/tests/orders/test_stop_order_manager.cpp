#include <gtest/gtest.h>

#include "../../include/order_book/book/tree_order_book.hpp"
#include "../../include/order_book/matching/fifo_matcher.hpp"
#include "../../include/order_book/orders/stop_order_manager.hpp"
#include "../../include/order_book/core/price_utils.hpp"

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// StopOrderManager tests
//
// StopOrderManager is driven entirely by on_trade(): the book reports a fill,
// the manager decides which pending stops that trade price triggers, converts
// them (STOP → MARKET, STOP_LIMIT → LIMIT at limit_price) and submits the
// converted orders to the book. These tests drive a real TreeOrderBook rather
// than a mock, because the trigger path's whole purpose is the interaction
// between the two — a triggered stop that produces a fill must be able to
// trigger further stops (the cascade).
// ─────────────────────────────────────────────────────────────────────────────

namespace order_book {

static Order make_order(uint64_t    id,
                        Side        side,
                        OrderType   type,
                        int64_t     price,
                        uint64_t    qty,
                        uint64_t    timestamp   = 1'000'000'000ULL,
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

class StopOrderManagerTest : public ::testing::Test {
protected:
    std::vector<FillEvent>    fills_;
    std::vector<CancelEvent>  cancels_;
    std::vector<AckEvent>     acks_;
    std::vector<TriggerEvent> triggers_;

    std::unique_ptr<TreeOrderBook>    book_;
    std::unique_ptr<StopOrderManager> stops_;

    uint64_t next_id_ = 9000;   // converted orders get IDs from this counter

    void SetUp() override {
        OrderBookCallbacks cbs;
        cbs.on_fill = [this](const FillEvent& e) {
            fills_.push_back(e);
            // Mirrors SimulationEngine::wire_callbacks(): a fill only RECORDS
            // a trade here. The book is mid-match at this point, so the
            // triggered orders are submitted by drain_triggers() once the book
            // call returns — see add_to_book() below.
            if (stops_) stops_->record_trade(e.fill_price, e.timestamp);
        };
        cbs.on_cancel = [this](const CancelEvent& e) { cancels_.push_back(e); };
        cbs.on_ack    = [this](const AckEvent& e)    { acks_.push_back(e); };

        book_ = std::make_unique<TreeOrderBook>(
            "AAPL", std::make_unique<FIFOMatcher>(), std::move(cbs));

        stops_ = std::make_unique<StopOrderManager>(
            *book_,
            [this](const TriggerEvent& e) { triggers_.push_back(e); },
            [this](const CancelEvent& e)  { cancels_.push_back(e); },
            [this]() -> uint64_t          { return next_id_++; });
    }

    void submit_stop(Order o) {
        stops_->submit(std::move(o), [this](const AckEvent& e) {
            acks_.push_back(e);
        });
    }

    // What SimulationEngine::dispatch() does: perform the book operation, then
    // act on whatever trades it reported.
    void add_to_book(Order o) {
        book_->add(std::move(o));
        stops_->drain_triggers();
    }
};

// ── Submission and acknowledgement ───────────────────────────────────────────

TEST_F(StopOrderManagerTest, AcceptedStopIsAckedPendingTrigger) {
    submit_stop(make_order(1, Side::ASK, OrderType::STOP,
                           to_basis_points(149.00), 100));

    ASSERT_EQ(acks_.size(), 1u);
    EXPECT_EQ(acks_[0].order_id, 1u);
    EXPECT_EQ(acks_[0].status, OrderStatus::PENDING_TRIGGER);
    EXPECT_EQ(stops_->pending_stop_count(), 1u);
    EXPECT_TRUE(stops_->has_stop(1));
}

TEST_F(StopOrderManagerTest, PendingStopsAreInvisibleToTheBook) {
    submit_stop(make_order(1, Side::ASK, OrderType::STOP,
                           to_basis_points(149.00), 100));
    submit_stop(make_order(2, Side::BID, OrderType::STOP,
                           to_basis_points(151.00), 100));

    // The whole reason stops live outside the book: they must not show up in
    // depth, quantity totals, or best bid/ask.
    EXPECT_EQ(book_->bid_depth(), 0u);
    EXPECT_EQ(book_->ask_depth(), 0u);
    EXPECT_EQ(book_->total_bid_qty(), 0u);
    EXPECT_EQ(book_->total_ask_qty(), 0u);
    EXPECT_FALSE(book_->best_bid().has_value());
    EXPECT_FALSE(book_->best_ask().has_value());
    EXPECT_FALSE(book_->has_order(1));
    EXPECT_FALSE(book_->has_order(2));
}

TEST_F(StopOrderManagerTest, StopLimitWithoutLimitPriceIsRejected) {
    submit_stop(make_order(1, Side::ASK, OrderType::STOP_LIMIT,
                           to_basis_points(149.00), 100,
                           1'000'000'000ULL, /*limit_price=*/0));

    ASSERT_EQ(acks_.size(), 1u);
    EXPECT_EQ(acks_[0].status, OrderStatus::REJECTED);
    EXPECT_EQ(stops_->pending_stop_count(), 0u);
}

TEST_F(StopOrderManagerTest, ZeroQuantityStopIsRejected) {
    submit_stop(make_order(1, Side::ASK, OrderType::STOP,
                           to_basis_points(149.00), /*qty=*/0));

    ASSERT_EQ(acks_.size(), 1u);
    EXPECT_EQ(acks_[0].status, OrderStatus::REJECTED);
    EXPECT_EQ(stops_->pending_stop_count(), 0u);
}

// ── Nearest-stop queries ─────────────────────────────────────────────────────

TEST_F(StopOrderManagerTest, NearestStopIsTheOneClosestToTriggering) {
    // Stop-buys trigger when the price RISES, so the lowest is nearest.
    submit_stop(make_order(1, Side::BID, OrderType::STOP, to_basis_points(152.00), 10));
    submit_stop(make_order(2, Side::BID, OrderType::STOP, to_basis_points(151.00), 10));
    // Stop-sells trigger when the price FALLS, so the highest is nearest.
    submit_stop(make_order(3, Side::ASK, OrderType::STOP, to_basis_points(148.00), 10));
    submit_stop(make_order(4, Side::ASK, OrderType::STOP, to_basis_points(149.00), 10));

    EXPECT_EQ(stops_->nearest_stop_buy(),  to_basis_points(151.00));
    EXPECT_EQ(stops_->nearest_stop_sell(), to_basis_points(149.00));
    EXPECT_EQ(stops_->pending_stop_count(), 4u);
}

TEST_F(StopOrderManagerTest, NearestStopIsZeroWhenNonePending) {
    EXPECT_EQ(stops_->nearest_stop_buy(),  0);
    EXPECT_EQ(stops_->nearest_stop_sell(), 0);
}

// ── Triggering ───────────────────────────────────────────────────────────────

TEST_F(StopOrderManagerTest, StopSellTriggersWhenTradePriceFallsToStop) {
    // Resting bid gives the triggered MARKET sell something to fill against.
    book_->add(make_order(10, Side::BID, OrderType::LIMIT,
                          to_basis_points(148.00), 500));

    submit_stop(make_order(1, Side::ASK, OrderType::STOP,
                           to_basis_points(149.00), 100));
    ASSERT_EQ(stops_->pending_stop_count(), 1u);

    // A trade at 149.00 is at-or-below the 149.00 stop → fires.
    stops_->on_trade(to_basis_points(149.00), 2'000'000'000ULL);

    ASSERT_EQ(triggers_.size(), 1u);
    EXPECT_EQ(triggers_[0].stop_order_id, 1u);
    EXPECT_EQ(triggers_[0].converted_to, OrderType::MARKET);
    EXPECT_EQ(triggers_[0].stop_price, to_basis_points(149.00));
    EXPECT_EQ(triggers_[0].trigger_trade_price, to_basis_points(149.00));
    EXPECT_NE(triggers_[0].triggered_order_id, 1u);   // a fresh ID is assigned

    // The converted MARKET sell hit the resting bid.
    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_price, to_basis_points(148.00));
    EXPECT_EQ(fills_[0].fill_quantity, 100u);
    EXPECT_EQ(fills_[0].aggressive_order_id, triggers_[0].triggered_order_id);

    EXPECT_EQ(stops_->pending_stop_count(), 0u);
    EXPECT_FALSE(stops_->has_stop(1));
}

TEST_F(StopOrderManagerTest, StopBuyTriggersWhenTradePriceRisesToStop) {
    book_->add(make_order(10, Side::ASK, OrderType::LIMIT,
                          to_basis_points(152.00), 500));

    submit_stop(make_order(1, Side::BID, OrderType::STOP,
                           to_basis_points(151.00), 100));

    stops_->on_trade(to_basis_points(151.50), 2'000'000'000ULL);

    ASSERT_EQ(triggers_.size(), 1u);
    EXPECT_EQ(triggers_[0].converted_to, OrderType::MARKET);
    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_price, to_basis_points(152.00));
    EXPECT_EQ(fills_[0].fill_quantity, 100u);
}

TEST_F(StopOrderManagerTest, StopDoesNotTriggerOnTheWrongSideOfTheStopPrice) {
    submit_stop(make_order(1, Side::ASK, OrderType::STOP,
                           to_basis_points(149.00), 100));
    submit_stop(make_order(2, Side::BID, OrderType::STOP,
                           to_basis_points(151.00), 100));

    // 150.00 is above the sell stop and below the buy stop — neither fires.
    stops_->on_trade(to_basis_points(150.00), 2'000'000'000ULL);

    EXPECT_TRUE(triggers_.empty());
    EXPECT_EQ(stops_->pending_stop_count(), 2u);
}

TEST_F(StopOrderManagerTest, StopLimitConvertsToLimitAtLimitPrice) {
    submit_stop(make_order(1, Side::ASK, OrderType::STOP_LIMIT,
                           to_basis_points(149.00), 100,
                           1'000'000'000ULL, /*limit_price=*/to_basis_points(148.50)));

    stops_->on_trade(to_basis_points(149.00), 2'000'000'000ULL);

    ASSERT_EQ(triggers_.size(), 1u);
    EXPECT_EQ(triggers_[0].converted_to, OrderType::LIMIT);

    // Nothing to fill against, so the converted LIMIT rests at limit_price —
    // not at the stop price.
    EXPECT_TRUE(fills_.empty());
    ASSERT_TRUE(book_->best_ask().has_value());
    EXPECT_EQ(*book_->best_ask(), to_basis_points(148.50));
    EXPECT_EQ(book_->total_ask_qty(), 100u);
}

TEST_F(StopOrderManagerTest, AllStopsAtAndBeyondTheTradePriceTriggerTogether) {
    // Deep bid at 148.00, so the converted sells fill AT 148.00 and don't
    // drag the trade price down into the 147.00 stop.
    book_->add(make_order(10, Side::BID, OrderType::LIMIT,
                          to_basis_points(148.00), 1000));

    submit_stop(make_order(1, Side::ASK, OrderType::STOP, to_basis_points(149.00), 10));
    submit_stop(make_order(2, Side::ASK, OrderType::STOP, to_basis_points(148.00), 20));
    submit_stop(make_order(3, Side::ASK, OrderType::STOP, to_basis_points(147.00), 30));

    // A trade at 148.00 fires the 149.00 and 148.00 stops, but not 147.00.
    stops_->on_trade(to_basis_points(148.00), 2'000'000'000ULL);

    EXPECT_EQ(triggers_.size(), 2u);
    EXPECT_EQ(stops_->pending_stop_count(), 1u);
    EXPECT_TRUE(stops_->has_stop(3));
}

TEST_F(StopOrderManagerTest, MultipleStopsAtTheSamePriceTriggerInSubmissionOrder) {
    book_->add(make_order(10, Side::BID, OrderType::LIMIT,
                          to_basis_points(148.00), 1000));

    submit_stop(make_order(1, Side::ASK, OrderType::STOP, to_basis_points(149.00), 10));
    submit_stop(make_order(2, Side::ASK, OrderType::STOP, to_basis_points(149.00), 20));
    submit_stop(make_order(3, Side::ASK, OrderType::STOP, to_basis_points(149.00), 30));

    stops_->on_trade(to_basis_points(149.00), 2'000'000'000ULL);

    ASSERT_EQ(triggers_.size(), 3u);
    EXPECT_EQ(triggers_[0].stop_order_id, 1u);
    EXPECT_EQ(triggers_[1].stop_order_id, 2u);
    EXPECT_EQ(triggers_[2].stop_order_id, 3u);
}

TEST_F(StopOrderManagerTest, TriggeredStopFillCascadesIntoFurtherStops) {
    // Two resting bids, one well below the other.
    book_->add(make_order(10, Side::BID, OrderType::LIMIT,
                          to_basis_points(148.00), 100));
    book_->add(make_order(11, Side::BID, OrderType::LIMIT,
                          to_basis_points(145.00), 100));

    // Stop A fires at 149.00; its MARKET sell fills at 148.00, which is
    // at-or-below stop B's 148.00 trigger — B must fire off A's fill.
    submit_stop(make_order(1, Side::ASK, OrderType::STOP, to_basis_points(149.00), 100));
    submit_stop(make_order(2, Side::ASK, OrderType::STOP, to_basis_points(148.00), 100));

    stops_->on_trade(to_basis_points(149.00), 2'000'000'000ULL);

    ASSERT_EQ(triggers_.size(), 2u);
    EXPECT_EQ(triggers_[0].stop_order_id, 1u);
    EXPECT_EQ(triggers_[1].stop_order_id, 2u);
    EXPECT_EQ(triggers_[1].trigger_trade_price, to_basis_points(148.00));

    // A's 100 at 148.00, then B's 100 at 145.00.
    ASSERT_EQ(fills_.size(), 2u);
    EXPECT_EQ(fills_[0].fill_price, to_basis_points(148.00));
    EXPECT_EQ(fills_[1].fill_price, to_basis_points(145.00));
    EXPECT_EQ(stops_->pending_stop_count(), 0u);

    // Book bookkeeping survived the cascade. Both resting bids were consumed
    // exactly once and no level was left behind or erased twice — the failure
    // mode when a triggered stop is submitted from inside the book's own
    // matching sweep instead of after it.
    EXPECT_EQ(book_->bid_depth(), 0u);
    EXPECT_EQ(book_->total_bid_qty(), 0u);
    EXPECT_FALSE(book_->has_order(10));
    EXPECT_FALSE(book_->has_order(11));
    EXPECT_FALSE(book_->best_bid().has_value());
}

TEST_F(StopOrderManagerTest, TriggeredStopWithNoLiquidityIsCancelledByTheBook) {
    // Empty book — a converted MARKET order has nothing to fill against.
    submit_stop(make_order(1, Side::ASK, OrderType::STOP,
                           to_basis_points(149.00), 100));

    stops_->on_trade(to_basis_points(149.00), 2'000'000'000ULL);

    ASSERT_EQ(triggers_.size(), 1u);
    EXPECT_TRUE(fills_.empty());
    ASSERT_EQ(cancels_.size(), 1u);
    EXPECT_EQ(cancels_[0].reason, CancelReason::MARKET_NO_LIQUIDITY);
    EXPECT_EQ(cancels_[0].order_id, triggers_[0].triggered_order_id);
}

// ── Cancellation ─────────────────────────────────────────────────────────────

TEST_F(StopOrderManagerTest, CancelRemovesPendingStopAndEmitsCancelEvent) {
    submit_stop(make_order(1, Side::ASK, OrderType::STOP,
                           to_basis_points(149.00), 100));
    cancels_.clear();

    stops_->cancel(1, 2'000'000'000ULL);

    EXPECT_EQ(stops_->pending_stop_count(), 0u);
    EXPECT_FALSE(stops_->has_stop(1));
    ASSERT_EQ(cancels_.size(), 1u);
    EXPECT_EQ(cancels_[0].order_id, 1u);
    EXPECT_EQ(cancels_[0].reason, CancelReason::STOP_CANCELLED);
    EXPECT_EQ(cancels_[0].remaining_quantity, 100u);

    // A cancelled stop must not fire afterwards.
    stops_->on_trade(to_basis_points(149.00), 3'000'000'000ULL);
    EXPECT_TRUE(triggers_.empty());
}

TEST_F(StopOrderManagerTest, CancelOfUnknownOrTriggeredStopIsASilentNoOp) {
    stops_->cancel(12345, 2'000'000'000ULL);
    EXPECT_TRUE(cancels_.empty());

    submit_stop(make_order(1, Side::ASK, OrderType::STOP,
                           to_basis_points(149.00), 100));
    stops_->on_trade(to_basis_points(149.00), 2'000'000'000ULL);
    ASSERT_EQ(triggers_.size(), 1u);

    const size_t cancels_before = cancels_.size();
    stops_->cancel(1, 3'000'000'000ULL);   // already triggered
    EXPECT_EQ(cancels_.size(), cancels_before);
}

TEST_F(StopOrderManagerTest, CancelLeavesOtherStopsAtTheSamePriceIntact) {
    submit_stop(make_order(1, Side::ASK, OrderType::STOP, to_basis_points(149.00), 10));
    submit_stop(make_order(2, Side::ASK, OrderType::STOP, to_basis_points(149.00), 20));

    stops_->cancel(1, 2'000'000'000ULL);

    EXPECT_EQ(stops_->pending_stop_count(), 1u);
    EXPECT_FALSE(stops_->has_stop(1));
    EXPECT_TRUE(stops_->has_stop(2));
    EXPECT_EQ(stops_->nearest_stop_sell(), to_basis_points(149.00));

    stops_->on_trade(to_basis_points(149.00), 3'000'000'000ULL);
    ASSERT_EQ(triggers_.size(), 1u);
    EXPECT_EQ(triggers_[0].stop_order_id, 2u);
}

} // namespace order_book
