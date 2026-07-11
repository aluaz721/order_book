#include <gtest/gtest.h>

#include "../../include/order_book/book/tree_order_book.hpp"
#include "../../include/order_book/matching/fifo_matcher.hpp"
#include "../../include/order_book/core/price_utils.hpp"

#include <vector>
#include <optional>
#include <string>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// Test fixture and helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace order_book {

// Convenience: build an Order with sane defaults so individual tests only
// set the fields they care about.
static Order make_order(uint64_t    id,
                        Side        side,
                        OrderType   type,
                        int64_t     price,
                        uint64_t    qty,
                        uint64_t    timestamp  = 1'000'000'000ULL,
                        std::string symbol     = "AAPL",
                        int64_t     limit_price = 0)
{
    Order o{};
    o.id                      = id;
    o.symbol                  = std::move(symbol);
    o.side                    = side;
    o.type                    = type;
    o.price                   = price;
    o.limit_price             = limit_price;
    o.quantity_remaining      = qty;
    o.orig_quantity           = qty;
    o.timestamp               = timestamp;
    o.status                  = OrderStatus::NEW;
    return o;
}

// Helper aliases so tests read naturally.
static Order bid_limit(uint64_t id, int64_t price_bp, uint64_t qty,
                       uint64_t ts = 1'000'000'000ULL)
{
    return make_order(id, Side::BID, OrderType::LIMIT, price_bp, qty, ts);
}

static Order ask_limit(uint64_t id, int64_t price_bp, uint64_t qty,
                       uint64_t ts = 1'000'000'000ULL)
{
    return make_order(id, Side::ASK, OrderType::LIMIT, price_bp, qty, ts);
}

static Order bid_market(uint64_t id, uint64_t qty,
                        uint64_t ts = 1'000'000'000ULL)
{
    return make_order(id, Side::BID, OrderType::MARKET, 0, qty, ts);
}

static Order ask_market(uint64_t id, uint64_t qty,
                        uint64_t ts = 1'000'000'000ULL)
{
    return make_order(id, Side::ASK, OrderType::MARKET, 0, qty, ts);
}

// ─────────────────────────────────────────────────────────────────────────────
// Fixture: owns a TreeOrderBook wired with recording callbacks
// ─────────────────────────────────────────────────────────────────────────────

class TreeOrderBookTest : public ::testing::Test {
protected:
    std::vector<FillEvent>   fills_;
    std::vector<CancelEvent> cancels_;
    std::vector<AckEvent>    acks_;
    std::vector<BookSnapshot> snapshots_;

    std::unique_ptr<TreeOrderBook> book_;

    void SetUp() override {
        fills_.clear();
        cancels_.clear();
        acks_.clear();
        snapshots_.clear();
        book_ = make_book();
    }

    std::unique_ptr<TreeOrderBook> make_book(std::string symbol = "AAPL") {
        OrderBookCallbacks cbs;
        cbs.on_fill = [this](const FillEvent& e) {
            fills_.push_back(e);
        };
        cbs.on_cancel = [this](const CancelEvent& e) {
            cancels_.push_back(e);
        };
        cbs.on_ack = [this](const AckEvent& e) {
            acks_.push_back(e);
        };
        cbs.on_book_update = [this](const BookSnapshot& s) {
            snapshots_.push_back(s);
        };
        return std::make_unique<TreeOrderBook>(
            std::move(symbol),
            std::make_unique<FIFOMatcher>(),
            std::move(cbs));
    }

    // Reset recording vectors between sub-scenarios in the same test.
    void clear_events() {
        fills_.clear(); cancels_.clear();
        acks_.clear();  snapshots_.clear();
    }

    uint64_t total_filled_qty() const {
        uint64_t total = 0;
        for (auto& f : fills_) total += f.fill_quantity;
        return total;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// 1. Construction and initial state
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TreeOrderBookTest, InitialStateIsEmpty) {
    EXPECT_EQ(book_->symbol(), "AAPL");
    EXPECT_EQ(book_->sequence(), 0u);
    EXPECT_EQ(book_->bid_depth(), 0u);
    EXPECT_EQ(book_->ask_depth(), 0u);
    EXPECT_EQ(book_->total_bid_qty(), 0u);
    EXPECT_EQ(book_->total_ask_qty(), 0u);
    EXPECT_EQ(book_->last_trade_price(), 0);
    EXPECT_FALSE(book_->best_bid().has_value());
    EXPECT_FALSE(book_->best_ask().has_value());
    EXPECT_FALSE(book_->spread().has_value());
    EXPECT_FALSE(book_->mid_price().has_value());
    EXPECT_FALSE(book_->weighted_mid().has_value());
}

TEST_F(TreeOrderBookTest, EmptySnapshotHasNoLevels) {
    auto snap = book_->snapshot(10);
    EXPECT_EQ(snap.symbol, "AAPL");
    EXPECT_TRUE(snap.bids.empty());
    EXPECT_TRUE(snap.asks.empty());
    EXPECT_EQ(snap.last_trade_price, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Resting limit orders — basic book state
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TreeOrderBookTest, SingleBidRestsAndIsQueryable) {
    book_->add(bid_limit(1, 1500000, 100));  // $150.00

    ASSERT_EQ(book_->bid_depth(), 1u);
    ASSERT_EQ(book_->ask_depth(), 0u);
    ASSERT_TRUE(book_->best_bid().has_value());
    EXPECT_EQ(book_->best_bid().value(), 1500000);
    EXPECT_EQ(book_->total_bid_qty(), 100u);
    EXPECT_TRUE(book_->has_order(1));
    EXPECT_FALSE(book_->has_order(99));

    // Ack emitted
    ASSERT_EQ(acks_.size(), 1u);
    EXPECT_EQ(acks_[0].order_id, 1u);
    EXPECT_EQ(acks_[0].status, OrderStatus::NEW);

    // No fills
    EXPECT_TRUE(fills_.empty());
}

TEST_F(TreeOrderBookTest, SingleAskRestsAndIsQueryable) {
    book_->add(ask_limit(1, 1510000, 50));  // $151.00

    ASSERT_EQ(book_->ask_depth(), 1u);
    ASSERT_EQ(book_->bid_depth(), 0u);
    ASSERT_TRUE(book_->best_ask().has_value());
    EXPECT_EQ(book_->best_ask().value(), 1510000);
    EXPECT_EQ(book_->total_ask_qty(), 50u);
}

TEST_F(TreeOrderBookTest, MultipleBidsOrderedBestFirst) {
    book_->add(bid_limit(1, 1490000, 100));  // $149.00
    book_->add(bid_limit(2, 1510000, 200));  // $151.00 — better
    book_->add(bid_limit(3, 1500000, 150));  // $150.00

    EXPECT_EQ(book_->best_bid().value(), 1510000);  // highest price = best bid
    EXPECT_EQ(book_->bid_depth(), 3u);
    EXPECT_EQ(book_->total_bid_qty(), 450u);

    auto snap = book_->snapshot(10);
    ASSERT_EQ(snap.bids.size(), 3u);
    EXPECT_EQ(snap.bids[0].price, 1510000);  // best first
    EXPECT_EQ(snap.bids[1].price, 1500000);
    EXPECT_EQ(snap.bids[2].price, 1490000);
}

TEST_F(TreeOrderBookTest, MultipleAsksOrderedBestFirst) {
    book_->add(ask_limit(1, 1510000, 100));  // $151.00
    book_->add(ask_limit(2, 1490000, 200));  // $149.00 — better
    book_->add(ask_limit(3, 1500000, 150));  // $150.00

    EXPECT_EQ(book_->best_ask().value(), 1490000);  // lowest price = best ask
    EXPECT_EQ(book_->ask_depth(), 3u);

    auto snap = book_->snapshot(10);
    ASSERT_EQ(snap.asks.size(), 3u);
    EXPECT_EQ(snap.asks[0].price, 1490000);  // best first
    EXPECT_EQ(snap.asks[1].price, 1500000);
    EXPECT_EQ(snap.asks[2].price, 1510000);
}

TEST_F(TreeOrderBookTest, MultipleOrdersAtSamePriceLevelAggregated) {
    book_->add(bid_limit(1, 1500000, 100));
    book_->add(bid_limit(2, 1500000, 50));
    book_->add(bid_limit(3, 1500000, 75));

    EXPECT_EQ(book_->bid_depth(), 1u);  // one price level
    EXPECT_EQ(book_->total_bid_qty(), 225u);

    auto snap = book_->snapshot(10);
    ASSERT_EQ(snap.bids.size(), 1u);
    EXPECT_EQ(snap.bids[0].quantity, 225u);
    EXPECT_EQ(snap.bids[0].order_count, 3u);
}

TEST_F(TreeOrderBookTest, SpreadAndMidPriceWithBothSides) {
    book_->add(bid_limit(1, 1500000, 100));  // $150.00
    book_->add(ask_limit(2, 1510000, 100));  // $151.00

    ASSERT_TRUE(book_->spread().has_value());
    EXPECT_EQ(book_->spread().value(), 10000);  // $1.00 = 10,000 bp

    ASSERT_TRUE(book_->mid_price().has_value());
    EXPECT_DOUBLE_EQ(book_->mid_price().value(), 1505000.0);
}

TEST_F(TreeOrderBookTest, WeightedMidPriceCalculation) {
    // bid 100 @ $150, ask 300 @ $152 → weighted mid closer to ask
    // wmid = (150*300 + 152*100) / 400 = (45000 + 15200) / 400 = 150.5
    book_->add(bid_limit(1, 1500000, 100));
    book_->add(ask_limit(2, 1520000, 300));

    ASSERT_TRUE(book_->weighted_mid().has_value());
    // wmid in basis points = (1500000*300 + 1520000*100) / 400 = 1505000
    EXPECT_DOUBLE_EQ(book_->weighted_mid().value(), 1505000.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Snapshot depth limiting
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TreeOrderBookTest, SnapshotRespectsDepthParameter) {
    for (int i = 1; i <= 5; ++i) {
        book_->add(bid_limit(i, 1500000 - i * 1000, 10));
    }

    auto snap2 = book_->snapshot(2);
    EXPECT_EQ(snap2.bids.size(), 2u);

    auto snap5 = book_->snapshot(5);
    EXPECT_EQ(snap5.bids.size(), 5u);

    auto snap10 = book_->snapshot(10);
    EXPECT_EQ(snap10.bids.size(), 5u);  // only 5 levels exist
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Matching — full fills
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TreeOrderBookTest, ExactMatchBidAgainstAsk) {
    book_->add(ask_limit(1, 1500000, 100));
    book_->add(bid_limit(2, 1500000, 100));

    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].aggressive_order_id, 2u);
    EXPECT_EQ(fills_[0].passive_order_id,    1u);
    EXPECT_EQ(fills_[0].fill_price,          1500000);
    EXPECT_EQ(fills_[0].fill_quantity,       100u);
    EXPECT_EQ(fills_[0].aggressor_side,      Side::BID);

    // Both orders fully consumed — book should be empty
    EXPECT_EQ(book_->bid_depth(), 0u);
    EXPECT_EQ(book_->ask_depth(), 0u);
    EXPECT_FALSE(book_->has_order(1));
    EXPECT_FALSE(book_->has_order(2));
    EXPECT_EQ(book_->last_trade_price(), 1500000);
}

TEST_F(TreeOrderBookTest, ExactMatchAskAgainstBid) {
    book_->add(bid_limit(1, 1500000, 100));
    book_->add(ask_limit(2, 1500000, 100));

    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].aggressive_order_id, 2u);
    EXPECT_EQ(fills_[0].passive_order_id,    1u);
    EXPECT_EQ(fills_[0].aggressor_side,      Side::ASK);
    EXPECT_EQ(book_->ask_depth(), 0u);
    EXPECT_EQ(book_->bid_depth(), 0u);
}

TEST_F(TreeOrderBookTest, AggressiveBidSweepsSingleAskLevel) {
    book_->add(ask_limit(1, 1500000, 40));
    book_->add(ask_limit(2, 1500000, 60));  // same price level, FIFO queue

    book_->add(bid_limit(3, 1500000, 100));

    // Two passive orders at same level — two fill events
    ASSERT_EQ(fills_.size(), 2u);
    EXPECT_EQ(fills_[0].passive_order_id, 1u);
    EXPECT_EQ(fills_[0].fill_quantity,    40u);
    EXPECT_EQ(fills_[1].passive_order_id, 2u);
    EXPECT_EQ(fills_[1].fill_quantity,    60u);

    EXPECT_EQ(total_filled_qty(), 100u);
    EXPECT_EQ(book_->ask_depth(), 0u);
    EXPECT_EQ(book_->bid_depth(), 0u);
}

TEST_F(TreeOrderBookTest, AggressiveBidSweepsMultipleAskLevels) {
    book_->add(ask_limit(1, 1500000, 50));  // $150.00
    book_->add(ask_limit(2, 1510000, 50));  // $151.00

    book_->add(bid_limit(3, 1520000, 100));  // crosses both levels

    ASSERT_EQ(fills_.size(), 2u);
    EXPECT_EQ(fills_[0].fill_price, 1500000);  // fills best ask first
    EXPECT_EQ(fills_[0].fill_quantity, 50u);
    EXPECT_EQ(fills_[1].fill_price, 1510000);
    EXPECT_EQ(fills_[1].fill_quantity, 50u);

    EXPECT_EQ(book_->ask_depth(), 0u);
    EXPECT_EQ(book_->bid_depth(), 0u);
}

TEST_F(TreeOrderBookTest, FillPriceIsMakerPriceNotAggresorsPrice) {
    // Passive ask at $150; aggressive bid at $155 — fill price must be $150
    book_->add(ask_limit(1, 1500000, 100));
    book_->add(bid_limit(2, 1550000, 100));

    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_price, 1500000);  // passive maker price
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Matching — partial fills
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TreeOrderBookTest, AggressiveOrderPartiallyFillsAndRests) {
    book_->add(ask_limit(1, 1500000, 40));
    book_->add(bid_limit(2, 1500000, 100));

    // Fills 40 from the ask, remainder 60 rests as a bid
    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_quantity, 40u);

    EXPECT_EQ(book_->ask_depth(), 0u);
    EXPECT_EQ(book_->bid_depth(), 1u);
    EXPECT_EQ(book_->total_bid_qty(), 60u);
    EXPECT_TRUE(book_->has_order(2));   // remainder is resting
    EXPECT_FALSE(book_->has_order(1));  // fully consumed
}

TEST_F(TreeOrderBookTest, PassiveOrderPartiallyFilledRemainsAtFront) {
    book_->add(ask_limit(1, 1500000, 100));
    book_->add(bid_limit(2, 1500000, 40));   // fills 40, passive order has 60 remaining

    // Passive order still resting with 60
    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_quantity, 40u);
    EXPECT_EQ(book_->ask_depth(), 1u);
    EXPECT_EQ(book_->total_ask_qty(), 60u);
    EXPECT_TRUE(book_->has_order(1));
    EXPECT_FALSE(book_->has_order(2));   // aggressive fully consumed

    // Now fill the remaining 60
    clear_events();
    book_->add(bid_limit(3, 1500000, 60));

    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].passive_order_id, 1u);
    EXPECT_EQ(fills_[0].fill_quantity, 60u);
    EXPECT_EQ(book_->ask_depth(), 0u);
}

TEST_F(TreeOrderBookTest, FIFOPriorityAtSamePriceLevel) {
    // Three asks at same price — earlier IDs should fill first
    book_->add(ask_limit(1, 1500000, 30));
    book_->add(ask_limit(2, 1500000, 30));
    book_->add(ask_limit(3, 1500000, 30));

    book_->add(bid_limit(4, 1500000, 50));

    // Should fill order 1 fully (30) then order 2 partially (20)
    ASSERT_EQ(fills_.size(), 2u);
    EXPECT_EQ(fills_[0].passive_order_id, 1u);
    EXPECT_EQ(fills_[0].fill_quantity,    30u);
    EXPECT_EQ(fills_[1].passive_order_id, 2u);
    EXPECT_EQ(fills_[1].fill_quantity,    20u);

    // Order 2 has 10 remaining, order 3 untouched
    EXPECT_EQ(book_->total_ask_qty(), 40u);  // 10 + 30
    EXPECT_FALSE(book_->has_order(1));
    EXPECT_TRUE(book_->has_order(2));
    EXPECT_TRUE(book_->has_order(3));
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Price crossing logic — orders that don't cross don't fill
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TreeOrderBookTest, BidBelowAskDoesNotCross) {
    book_->add(ask_limit(1, 1510000, 100));  // $151.00
    book_->add(bid_limit(2, 1500000, 100));  // $150.00 — below ask

    EXPECT_TRUE(fills_.empty());
    EXPECT_EQ(book_->ask_depth(), 1u);
    EXPECT_EQ(book_->bid_depth(), 1u);
}

TEST_F(TreeOrderBookTest, AskAboveBidDoesNotCross) {
    book_->add(bid_limit(1, 1500000, 100));  // $150.00
    book_->add(ask_limit(2, 1510000, 100));  // $151.00 — above bid

    EXPECT_TRUE(fills_.empty());
    EXPECT_EQ(book_->bid_depth(), 1u);
    EXPECT_EQ(book_->ask_depth(), 1u);
}

TEST_F(TreeOrderBookTest, BidCrossesAtExactAskPrice) {
    book_->add(ask_limit(1, 1500000, 100));
    book_->add(bid_limit(2, 1500000, 100));

    EXPECT_EQ(fills_.size(), 1u);
}

TEST_F(TreeOrderBookTest, BidAboveAskCrosses) {
    book_->add(ask_limit(1, 1500000, 100));
    book_->add(bid_limit(2, 1550000, 100));

    EXPECT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_price, 1500000);  // fill at passive price
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. Cancel
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TreeOrderBookTest, CancelRemovesOrderFromBook) {
    book_->add(bid_limit(1, 1500000, 100));
    ASSERT_TRUE(book_->has_order(1));

    clear_events();
    book_->cancel(1);

    EXPECT_FALSE(book_->has_order(1));
    EXPECT_EQ(book_->bid_depth(), 0u);
    EXPECT_EQ(book_->total_bid_qty(), 0u);

    ASSERT_EQ(cancels_.size(), 1u);
    EXPECT_EQ(cancels_[0].order_id,           1u);
    EXPECT_EQ(cancels_[0].reason,             CancelReason::CLIENT_REQUEST);
    EXPECT_EQ(cancels_[0].remaining_quantity, 100u);
}

TEST_F(TreeOrderBookTest, CancelUnknownOrderIsNoOp) {
    book_->cancel(999);  // never submitted
    EXPECT_TRUE(cancels_.empty());
}

TEST_F(TreeOrderBookTest, CancelAlreadyFilledOrderIsNoOp) {
    book_->add(ask_limit(1, 1500000, 100));
    book_->add(bid_limit(2, 1500000, 100));  // fills order 1

    clear_events();
    book_->cancel(1);  // order 1 is already gone

    EXPECT_TRUE(cancels_.empty());
}

TEST_F(TreeOrderBookTest, CancelMiddleOrderPreservesOthersAtLevel) {
    book_->add(bid_limit(1, 1500000, 100));
    book_->add(bid_limit(2, 1500000, 200));
    book_->add(bid_limit(3, 1500000, 150));

    book_->cancel(2);

    EXPECT_FALSE(book_->has_order(2));
    EXPECT_TRUE(book_->has_order(1));
    EXPECT_TRUE(book_->has_order(3));
    EXPECT_EQ(book_->total_bid_qty(), 250u);
    EXPECT_EQ(book_->bid_depth(), 1u);  // level still exists
}

TEST_F(TreeOrderBookTest, CancelLastOrderAtLevelRemovesLevel) {
    book_->add(bid_limit(1, 1500000, 100));
    book_->cancel(1);

    EXPECT_EQ(book_->bid_depth(), 0u);
    EXPECT_FALSE(book_->best_bid().has_value());
}

TEST_F(TreeOrderBookTest, CancelPartiallyFilledOrderCancelsRemainder) {
    book_->add(ask_limit(1, 1500000, 100));
    book_->add(bid_limit(2, 1500000, 40));  // fills 40 from ask 1

    clear_events();
    book_->cancel(1);  // cancel the remaining 60

    ASSERT_EQ(cancels_.size(), 1u);
    EXPECT_EQ(cancels_[0].order_id,           1u);
    EXPECT_EQ(cancels_[0].remaining_quantity, 60u);
    EXPECT_EQ(book_->ask_depth(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. Market orders
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TreeOrderBookTest, MarketBidFullyFillsAgainstResting) {
    book_->add(ask_limit(1, 1500000, 100));
    book_->add(bid_market(2, 100));

    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_quantity, 100u);
    EXPECT_EQ(book_->ask_depth(), 0u);
}

TEST_F(TreeOrderBookTest, MarketAskFullyFillsAgainstResting) {
    book_->add(bid_limit(1, 1500000, 100));
    book_->add(ask_market(2, 100));

    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_quantity, 100u);
    EXPECT_EQ(book_->bid_depth(), 0u);
}

TEST_F(TreeOrderBookTest, MarketOrderWithInsufficientLiquidityCancelsRemainder) {
    book_->add(ask_limit(1, 1500000, 40));
    book_->add(bid_market(2, 100));

    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_quantity, 40u);

    // Remaining 60 is cancelled, not rested
    ASSERT_EQ(cancels_.size(), 1u);
    EXPECT_EQ(cancels_[0].order_id,           2u);
    EXPECT_EQ(cancels_[0].reason,             CancelReason::MARKET_NO_LIQUIDITY);
    EXPECT_EQ(cancels_[0].remaining_quantity, 60u);

    EXPECT_EQ(book_->bid_depth(), 0u);  // market order never rests
}

TEST_F(TreeOrderBookTest, MarketOrderOnEmptyBookCancelsImmediately) {
    book_->add(bid_market(1, 100));

    EXPECT_TRUE(fills_.empty());
    ASSERT_EQ(cancels_.size(), 1u);
    EXPECT_EQ(cancels_[0].reason, CancelReason::MARKET_NO_LIQUIDITY);
    EXPECT_EQ(book_->bid_depth(), 0u);
}

TEST_F(TreeOrderBookTest, MarketOrderSweepsMultipleLevels) {
    book_->add(ask_limit(1, 1500000, 40));
    book_->add(ask_limit(2, 1510000, 30));
    book_->add(ask_limit(3, 1520000, 30));

    book_->add(bid_market(4, 100));

    ASSERT_EQ(fills_.size(), 3u);
    EXPECT_EQ(total_filled_qty(), 100u);
    EXPECT_EQ(book_->ask_depth(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 9. IOC (Immediate-Or-Cancel)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TreeOrderBookTest, IOCFullyFillsIfLiquidityExists) {
    book_->add(ask_limit(1, 1500000, 100));

    auto ioc = make_order(2, Side::BID, OrderType::IMMEDIATE_OR_CANCEL,
                          1500000, 100);
    book_->add(ioc);

    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_quantity, 100u);
    EXPECT_TRUE(cancels_.empty());  // nothing to cancel
}

TEST_F(TreeOrderBookTest, IOCPartiallyFillsThenCancelsRemainder) {
    book_->add(ask_limit(1, 1500000, 40));

    auto ioc = make_order(2, Side::BID, OrderType::IMMEDIATE_OR_CANCEL,
                          1500000, 100);
    book_->add(ioc);

    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_quantity, 40u);

    ASSERT_EQ(cancels_.size(), 1u);
    EXPECT_EQ(cancels_[0].order_id,           2u);
    EXPECT_EQ(cancels_[0].reason,             CancelReason::IOC_EXPIRED);
    EXPECT_EQ(cancels_[0].remaining_quantity, 60u);

    EXPECT_EQ(book_->bid_depth(), 0u);  // IOC never rests
}

TEST_F(TreeOrderBookTest, IOCWithNoCrossableLiquidityCancelsEntirely) {
    book_->add(ask_limit(1, 1510000, 100));  // ask at $151

    auto ioc = make_order(2, Side::BID, OrderType::IMMEDIATE_OR_CANCEL,
                          1500000, 100);  // bid at $150 — doesn't cross
    book_->add(ioc);

    EXPECT_TRUE(fills_.empty());
    ASSERT_EQ(cancels_.size(), 1u);
    EXPECT_EQ(cancels_[0].reason,             CancelReason::IOC_EXPIRED);
    EXPECT_EQ(cancels_[0].remaining_quantity, 100u);
    EXPECT_EQ(book_->bid_depth(), 0u);
}

TEST_F(TreeOrderBookTest, IOCNeverRestsInBook) {
    // No liquidity at all
    auto ioc = make_order(1, Side::BID, OrderType::IMMEDIATE_OR_CANCEL,
                          1500000, 100);
    book_->add(ioc);

    EXPECT_TRUE(fills_.empty());
    EXPECT_EQ(book_->bid_depth(), 0u);
    EXPECT_FALSE(book_->has_order(1));
}

// ─────────────────────────────────────────────────────────────────────────────
// 10. FOK (Fill-Or-Kill)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TreeOrderBookTest, FOKFullyFillsWhenLiquiditySufficient) {
    book_->add(ask_limit(1, 1500000, 100));

    auto fok = make_order(2, Side::BID, OrderType::FILL_OR_KILL,
                          1500000, 100);
    book_->add(fok);

    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_quantity, 100u);
    EXPECT_TRUE(cancels_.empty());
    EXPECT_EQ(book_->ask_depth(), 0u);
}

TEST_F(TreeOrderBookTest, FOKCancelledWhenInsufficientLiquidity) {
    book_->add(ask_limit(1, 1500000, 40));  // only 40 available

    auto fok = make_order(2, Side::BID, OrderType::FILL_OR_KILL,
                          1500000, 100);  // needs 100
    book_->add(fok);

    // FOK pre-check must have fired — no fills should occur
    EXPECT_TRUE(fills_.empty());
    ASSERT_EQ(cancels_.size(), 1u);
    EXPECT_EQ(cancels_[0].order_id, 2u);
    EXPECT_EQ(cancels_[0].reason,   CancelReason::FOK_FAILED);

    // Passive order 1 must be untouched
    EXPECT_TRUE(book_->has_order(1));
    EXPECT_EQ(book_->total_ask_qty(), 40u);
}

TEST_F(TreeOrderBookTest, FOKCancelledWhenNoLiquidity) {
    auto fok = make_order(1, Side::BID, OrderType::FILL_OR_KILL,
                          1500000, 100);
    book_->add(fok);

    EXPECT_TRUE(fills_.empty());
    ASSERT_EQ(cancels_.size(), 1u);
    EXPECT_EQ(cancels_[0].reason, CancelReason::FOK_FAILED);
}

TEST_F(TreeOrderBookTest, FOKChecksLiquidityAcrossMultipleLevels) {
    // 60 at $150, 60 at $151 — total 120, enough for FOK of 100
    book_->add(ask_limit(1, 1500000, 60));
    book_->add(ask_limit(2, 1510000, 60));

    auto fok = make_order(3, Side::BID, OrderType::FILL_OR_KILL,
                          1510000, 100);
    book_->add(fok);

    ASSERT_EQ(fills_.size(), 2u);
    EXPECT_EQ(total_filled_qty(), 100u);
    EXPECT_TRUE(cancels_.empty());
}

TEST_F(TreeOrderBookTest, FOKNeverRestsInBook) {
    book_->add(ask_limit(1, 1500000, 40));

    auto fok = make_order(2, Side::BID, OrderType::FILL_OR_KILL,
                          1500000, 100);
    book_->add(fok);

    EXPECT_EQ(book_->bid_depth(), 0u);
    EXPECT_FALSE(book_->has_order(2));
}

// ─────────────────────────────────────────────────────────────────────────────
// 11. Market-Limit orders
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TreeOrderBookTest, MarketLimitFullyFillsLikeMarket) {
    book_->add(ask_limit(1, 1500000, 100));

    auto ml = make_order(2, Side::BID, OrderType::MARKET_LIMIT, 0, 100);
    book_->add(ml);

    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_quantity, 100u);
    EXPECT_EQ(book_->ask_depth(), 0u);
    EXPECT_EQ(book_->bid_depth(), 0u);
}

TEST_F(TreeOrderBookTest, MarketLimitRestsRemainderAtLastTradePrice) {
    book_->add(ask_limit(1, 1500000, 40));  // fills 40 at $150

    auto ml = make_order(2, Side::BID, OrderType::MARKET_LIMIT, 0, 100);
    book_->add(ml);

    // 40 filled; remaining 60 should rest at last trade price $150
    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_quantity, 40u);

    EXPECT_EQ(book_->bid_depth(), 1u);
    EXPECT_EQ(book_->total_bid_qty(), 60u);
    EXPECT_TRUE(book_->has_order(2));
    EXPECT_EQ(book_->best_bid().value(), 1500000);
}

// ─────────────────────────────────────────────────────────────────────────────
// 12. replace()
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TreeOrderBookTest, ReplaceUpdatesOrderInBook) {
    book_->add(bid_limit(1, 1500000, 100));
    ASSERT_TRUE(book_->has_order(1));

    auto new_order = bid_limit(2, 1510000, 50);
    book_->replace(1, new_order);

    EXPECT_FALSE(book_->has_order(1));
    EXPECT_TRUE(book_->has_order(2));
    EXPECT_EQ(book_->best_bid().value(), 1510000);
    EXPECT_EQ(book_->total_bid_qty(), 50u);
}

TEST_F(TreeOrderBookTest, ReplacedOrderLosesTimePriority) {
    // Order 1 and 3 at same price; replace 1 with new order 2
    // Order 2 should go behind order 3 in the queue
    book_->add(bid_limit(1, 1500000, 20));
    book_->add(bid_limit(3, 1500000, 20));

    // Replace order 1 with order 2 at same price
    auto replacement = bid_limit(2, 1500000, 20);
    book_->replace(1, replacement);

    clear_events();

    // Hit the bids with an ask: order 3 should fill before order 2 (time priority)
    book_->add(ask_limit(99, 1500000, 25));

    ASSERT_GE(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].passive_order_id, 3u);  // 3 came before 2 (replacement)
}

// ─────────────────────────────────────────────────────────────────────────────
// 13. execute() — feed-driven fills
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TreeOrderBookTest, ExecuteFullyFillsRestingOrder) {
    book_->add(ask_limit(1, 1500000, 100));
    clear_events();

    book_->execute(1, 100, 2'000'000'000ULL);

    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].passive_order_id, 1u);
    EXPECT_EQ(fills_[0].fill_quantity,    100u);
    EXPECT_FALSE(book_->has_order(1));
    EXPECT_EQ(book_->ask_depth(), 0u);
}

TEST_F(TreeOrderBookTest, ExecutePartiallyFillsRestingOrder) {
    book_->add(ask_limit(1, 1500000, 100));
    clear_events();

    book_->execute(1, 40, 2'000'000'000ULL);

    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_quantity, 40u);
    EXPECT_TRUE(book_->has_order(1));
    EXPECT_EQ(book_->total_ask_qty(), 60u);
}

TEST_F(TreeOrderBookTest, ExecuteUnknownOrderIsNoOp) {
    book_->execute(999, 100, 0);
    EXPECT_TRUE(fills_.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// 14. Callbacks and sequence numbers
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TreeOrderBookTest, AckEmittedForEveryAdd) {
    book_->add(bid_limit(1, 1500000, 100));
    book_->add(ask_limit(2, 1510000, 100));
    book_->add(bid_market(3, 50));

    EXPECT_EQ(acks_.size(), 3u);
    EXPECT_EQ(acks_[0].order_id, 1u);
    EXPECT_EQ(acks_[1].order_id, 2u);
    EXPECT_EQ(acks_[2].order_id, 3u);
}

TEST_F(TreeOrderBookTest, SnapshotEmittedAfterEveryStateChange) {
    EXPECT_EQ(snapshots_.size(), 0u);
    book_->add(bid_limit(1, 1500000, 100));
    EXPECT_EQ(snapshots_.size(), 1u);
    book_->add(ask_limit(2, 1510000, 100));
    EXPECT_EQ(snapshots_.size(), 2u);
    book_->cancel(1);
    EXPECT_EQ(snapshots_.size(), 3u);
}

TEST_F(TreeOrderBookTest, SnapshotSequenceIsMonotonicallyIncreasing) {
    book_->add(bid_limit(1, 1500000, 100));
    book_->add(ask_limit(2, 1510000, 100));
    book_->add(bid_limit(3, 1490000, 50));

    ASSERT_GE(snapshots_.size(), 3u);
    for (size_t i = 1; i < snapshots_.size(); ++i) {
        EXPECT_GT(snapshots_[i].sequence, snapshots_[i - 1].sequence)
            << "sequence not monotonically increasing at index " << i;
    }
}

TEST_F(TreeOrderBookTest, FillSequenceIsMonotonicallyIncreasing) {
    // Create a scenario with multiple fills
    for (int i = 1; i <= 5; ++i) {
        book_->add(ask_limit(i, 1500000, 10));
    }
    book_->add(bid_limit(10, 1500000, 50));

    ASSERT_EQ(fills_.size(), 5u);
    for (size_t i = 1; i < fills_.size(); ++i) {
        EXPECT_GT(fills_[i].sequence, fills_[i - 1].sequence)
            << "fill sequence not monotonically increasing at index " << i;
    }
}

TEST_F(TreeOrderBookTest, LastTradePriceUpdatesAfterFill) {
    EXPECT_EQ(book_->last_trade_price(), 0);

    book_->add(ask_limit(1, 1500000, 100));
    book_->add(bid_limit(2, 1500000, 100));

    EXPECT_EQ(book_->last_trade_price(), 1500000);

    book_->add(ask_limit(3, 1520000, 100));
    book_->add(bid_limit(4, 1520000, 100));

    EXPECT_EQ(book_->last_trade_price(), 1520000);
}

// ─────────────────────────────────────────────────────────────────────────────
// 15. set_matching_algorithm() swap
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TreeOrderBookTest, CanSwapMatchingAlgorithmOnEmptyBook) {
    // Swap on empty book should not throw
    EXPECT_NO_THROW(
        book_->set_matching_algorithm(std::make_unique<FIFOMatcher>())
    );
}

TEST_F(TreeOrderBookTest, SwapMatchingAlgorithmOnNonEmptyBookThrows) {
    book_->add(bid_limit(1, 1500000, 100));
    EXPECT_THROW(
        book_->set_matching_algorithm(std::make_unique<FIFOMatcher>()),
        std::logic_error
    );
}

// ─────────────────────────────────────────────────────────────────────────────
// 16. set_callbacks() late binding
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TreeOrderBookTest, CallbacksCanBeReplacedAfterConstruction) {
    std::vector<FillEvent> new_fills;
    OrderBookCallbacks new_cbs;
    new_cbs.on_fill = [&new_fills](const FillEvent& e) {
        new_fills.push_back(e);
    };
    book_->set_callbacks(std::move(new_cbs));

    book_->add(ask_limit(1, 1500000, 100));
    book_->add(bid_limit(2, 1500000, 100));

    // Original fill recorder should be empty; new one should have the fill
    EXPECT_TRUE(fills_.empty());
    ASSERT_EQ(new_fills.size(), 1u);
    EXPECT_EQ(new_fills[0].fill_quantity, 100u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 17. Edge cases and stress
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TreeOrderBookTest, QuantityOfOneFilledExactly) {
    book_->add(ask_limit(1, 1500000, 1));
    book_->add(bid_limit(2, 1500000, 1));

    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_quantity, 1u);
    EXPECT_EQ(book_->ask_depth(), 0u);
    EXPECT_EQ(book_->bid_depth(), 0u);
}

TEST_F(TreeOrderBookTest, VeryHighPriceHandledCorrectly) {
    // $999,999.99 = 9,999,999,900 basis points — fits in int64_t
    const int64_t high_price = 9'999'999'900LL;
    book_->add(ask_limit(1, high_price, 1));
    book_->add(bid_limit(2, high_price, 1));

    ASSERT_EQ(fills_.size(), 1u);
    EXPECT_EQ(fills_[0].fill_price, high_price);
}

TEST_F(TreeOrderBookTest, ManyOrdersCancelledAndRefilled) {
    // Add 100 bids, cancel half, verify state
    for (uint64_t i = 1; i <= 100; ++i) {
        book_->add(bid_limit(i, 1500000 - static_cast<int64_t>(i) * 100, 10));
    }
    EXPECT_EQ(book_->bid_depth(), 100u);

    for (uint64_t i = 1; i <= 50; ++i) {
        book_->cancel(i);
    }
    EXPECT_EQ(book_->bid_depth(), 50u);
    EXPECT_EQ(book_->total_bid_qty(), 500u);

    // Add a sweeping ask
    book_->add(ask_market(200, 500));
    EXPECT_EQ(total_filled_qty(), 500u);
    EXPECT_EQ(book_->bid_depth(), 0u);
}

TEST_F(TreeOrderBookTest, HasOrderReturnsFalseAfterCancelAndFill) {
    book_->add(ask_limit(1, 1500000, 50));
    book_->add(ask_limit(2, 1510000, 50));

    // Fill order 1 via matching
    book_->add(bid_limit(3, 1500000, 50));
    EXPECT_FALSE(book_->has_order(1));

    // Cancel order 2
    book_->cancel(2);
    EXPECT_FALSE(book_->has_order(2));
}

TEST_F(TreeOrderBookTest, TotalQuantitiesAreAccurateAfterMultipleOps) {
    book_->add(bid_limit(1, 1500000, 100));
    book_->add(bid_limit(2, 1490000, 200));
    book_->add(bid_limit(3, 1480000, 150));

    EXPECT_EQ(book_->total_bid_qty(), 450u);

    book_->cancel(2);
    EXPECT_EQ(book_->total_bid_qty(), 250u);

    // Add ask that partially fills order 1
    book_->add(ask_limit(4, 1500000, 60));
    EXPECT_EQ(book_->total_bid_qty(), 190u);  // 40 remaining from order 1 + 150 from order 3
}

TEST_F(TreeOrderBookTest, SnapshotContainsCorrectPriceAndQuantity) {
    book_->add(bid_limit(1, 1500000, 100));
    book_->add(bid_limit(2, 1500000, 50));   // same level
    book_->add(bid_limit(3, 1490000, 200));
    book_->add(ask_limit(4, 1510000, 75));

    auto snap = book_->snapshot(10);

    ASSERT_EQ(snap.bids.size(), 2u);
    EXPECT_EQ(snap.bids[0].price,       1500000);
    EXPECT_EQ(snap.bids[0].quantity,    150u);
    EXPECT_EQ(snap.bids[0].order_count, 2u);
    EXPECT_EQ(snap.bids[1].price,       1490000);
    EXPECT_EQ(snap.bids[1].quantity,    200u);

    ASSERT_EQ(snap.asks.size(), 1u);
    EXPECT_EQ(snap.asks[0].price,    1510000);
    EXPECT_EQ(snap.asks[0].quantity, 75u);
}

} // namespace order_book