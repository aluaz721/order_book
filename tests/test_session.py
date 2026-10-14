"""
Tests for order_book.Session — the façade over engine + book + order queue.

The point of Session is that every one of the seven order types is reachable
from Python in one call, in dollars, so these tests are organised by order
type. They are also the readable statement of what each type does, which is
why they assert on book state and fill prices rather than just on counts.
"""

import pytest

import order_book as ob


@pytest.fixture
def session():
    return ob.Session("AAPL")


@pytest.fixture
def market(session):
    """A two-sided book: 100 bid at 149.50, 100 offered at 150.50."""
    session.limit("buy", 100, 149.50)
    session.limit("sell", 100, 150.50)
    return session


# ── LIMIT ────────────────────────────────────────────────────────────────────

def test_limit_rests_when_it_does_not_cross(session):
    result = session.limit("buy", 100, 150.00)

    assert result.status == ob.OrderStatus.NEW
    assert result.filled_quantity == 0
    assert session.best_bid() == 150.00
    assert session.best_ask() is None
    assert session.book.total_bid_qty() == 100


def test_limit_fills_against_a_crossing_order(market):
    result = market.limit("sell", 40, 149.50)

    assert result.filled_quantity == 40
    assert len(result.fills) == 1
    assert ob.from_basis_points(result.fills[0].fill_price) == 149.50
    assert market.book.total_bid_qty() == 60


def test_limit_price_is_required(session):
    with pytest.raises(ValueError, match="price"):
        session.submit("buy", "limit", 100)


# ── MARKET ───────────────────────────────────────────────────────────────────

def test_market_fills_at_the_touch_and_cancels_the_remainder(market):
    result = market.market("buy", 150)

    assert result.filled_quantity == 100
    assert ob.from_basis_points(result.fills[0].fill_price) == 150.50
    assert len(result.cancels) == 1
    assert result.cancels[0].reason == ob.CancelReason.MARKET_NO_LIQUIDITY
    assert result.cancels[0].remaining_quantity == 50
    assert market.best_ask() is None


def test_market_needs_no_price(session):
    session.limit("sell", 10, 150.00)
    result = session.market("buy", 10)
    assert result.filled_quantity == 10


# ── MARKET_LIMIT ─────────────────────────────────────────────────────────────

def test_market_limit_rests_its_remainder_at_the_traded_price(market):
    # The price passed is deliberately far from the market: matching ignores
    # it, and the remainder rests at the price that actually traded.
    result = market.market_limit("buy", 150, 160.00)

    assert result.filled_quantity == 100
    assert market.best_bid() == 150.50      # the traded price, not 160.00
    assert market.book.total_bid_qty() == 150   # 100 original + 50 remainder


# ── IOC ──────────────────────────────────────────────────────────────────────

def test_ioc_fills_what_it_can_and_cancels_the_rest(market):
    result = market.ioc("buy", 150, 150.50)

    assert result.filled_quantity == 100
    assert result.cancels[0].reason == ob.CancelReason.IOC_EXPIRED
    assert result.cancels[0].remaining_quantity == 50
    # Nothing rests — the defining difference from a LIMIT order.
    assert market.book.total_bid_qty() == 100   # just the original bid


def test_ioc_does_not_cross_above_its_price(market):
    result = market.ioc("buy", 50, 150.00)   # best ask is 150.50

    assert result.filled_quantity == 0
    assert result.cancels[0].reason == ob.CancelReason.IOC_EXPIRED
    assert market.book.total_ask_qty() == 100


# ── FOK ──────────────────────────────────────────────────────────────────────

def test_fok_fills_completely_or_not_at_all(market):
    filled = market.fok("buy", 100, 150.50)
    assert filled.filled_quantity == 100

    market.limit("sell", 40, 150.50)
    short = market.fok("buy", 100, 150.50)

    assert short.rejected
    assert "shortfall" in short.message
    assert short.filled_quantity == 0
    # Rejected before matching, so the resting ask is untouched.
    assert market.book.total_ask_qty() == 40


# ── STOP ─────────────────────────────────────────────────────────────────────

def test_stop_is_held_outside_the_book_until_a_trade_crosses_it(market):
    result = market.stop("sell", 50, stop_price=149.00)

    assert result.pending_trigger
    assert market.pending_stops == 1
    assert market.nearest_stop("sell") == 149.00
    # Not in the book: no effect on depth, quantity or the touch.
    assert market.book.total_ask_qty() == 100
    assert not market.book.has_order(result.order_id)

    # Sweep down through the stop: fills at 149.50, then 148.50 — the latter
    # is at-or-below 149.00, so the stop fires.
    market.limit("buy", 200, 148.50)
    swept = market.limit("sell", 150, 148.00)

    assert len(swept.triggers) == 1
    assert swept.triggers[0].converted_to == ob.OrderType.MARKET
    assert market.pending_stops == 0
    assert market.engine.stops_triggered() == 1

    # The stop's own MARKET sell traded, on top of the sweep itself.
    assert swept.filled_quantity == 150
    assert swept.traded_quantity == 200


def test_stop_buy_fires_when_the_market_rises(market):
    market.stop("buy", 50, stop_price=151.00)
    assert market.nearest_stop("buy") == 151.00

    market.limit("sell", 300, 151.50)
    result = market.limit("buy", 250, 151.50)

    assert len(result.triggers) == 1
    assert ob.from_basis_points(result.triggers[0].trigger_trade_price) == 151.50
    assert market.pending_stops == 0


def test_stop_that_would_fire_immediately_is_rejected(market):
    # A sell stop at 150.00 sits above the 149.50 bid, so any trade at the bid
    # would fire it at once — that is not a stop.
    result = market.stop("sell", 50, stop_price=150.00)

    assert result.rejected
    assert "above the current market" in result.message or \
           "below the current market" in result.message
    assert market.pending_stops == 0
    assert market.engine.orders_rejected() == 1


def test_stop_needs_a_stop_price(session):
    with pytest.raises(ValueError, match="stop_price"):
        session.submit("sell", "stop", 50)


# ── STOP_LIMIT ───────────────────────────────────────────────────────────────

def test_stop_limit_becomes_a_limit_order_at_its_limit_price(market):
    result = market.stop_limit("sell", 50, stop_price=149.00, limit_price=149.50)
    assert result.pending_trigger

    market.limit("buy", 200, 148.50)
    swept = market.limit("sell", 150, 148.00)

    assert len(swept.triggers) == 1
    assert swept.triggers[0].converted_to == ob.OrderType.LIMIT
    # Converted to a LIMIT sell at 149.50, which does not cross the remaining
    # 148.50 bid — so it rests, where a STOP would have swept.
    assert market.best_ask() == 149.50
    assert market.book.has_order(swept.triggers[0].triggered_order_id)


def test_stop_limit_with_its_limit_on_the_wrong_side_is_rejected(market):
    # Buy-stop convention: limit_price <= stop_price.
    result = market.stop_limit("buy", 50, stop_price=151.00, limit_price=152.00)

    assert result.rejected
    assert "limit_price" in result.message
    assert market.pending_stops == 0


def test_stop_limit_needs_a_limit_price(session):
    with pytest.raises(ValueError, match="limit_price"):
        session.submit("sell", "stop_limit", 50, stop_price=149.00)


# ── Cancellation ─────────────────────────────────────────────────────────────

def test_cancel_removes_a_resting_order(session):
    result = session.limit("buy", 100, 150.00)

    assert session.cancel(result.order_id) is True
    assert not session.book.has_order(result.order_id)
    assert session.best_bid() is None
    assert session.cancels[-1].reason == ob.CancelReason.CLIENT_REQUEST


def test_cancel_removes_a_pending_stop(market):
    result = market.stop("sell", 50, stop_price=149.00)

    assert market.cancel(result.order_id) is True
    assert market.pending_stops == 0
    assert market.cancels[-1].reason == ob.CancelReason.STOP_CANCELLED

    # And it stays gone when the market later crosses its stop price.
    market.limit("buy", 200, 148.50)
    swept = market.limit("sell", 150, 148.00)
    assert swept.triggers == ()


def test_cancel_of_an_unknown_id_reports_false(session):
    assert session.cancel(999) is False


# ── Batching, strategy, housekeeping ─────────────────────────────────────────

def test_submissions_can_be_batched_before_draining(session):
    session.limit("sell", 10, 150.00, run=False)
    session.limit("sell", 10, 150.00, run=False)
    assert session.source.pending_count() == 2
    assert session.book.total_ask_qty() == 0

    session.run()
    assert session.book.total_ask_qty() == 20
    assert session.engine.orders_processed() == 2


def test_order_ids_are_assigned_and_do_not_collide_with_triggered_orders(market):
    first = market.limit("buy", 10, 148.00)
    second = market.limit("buy", 10, 148.00)

    assert second.order_id == first.order_id + 1
    assert first.order_id < 1_000_000   # engine-generated ids start there


def test_strategy_callback_quotes_on_book_updates(session):
    seen = []

    def strategy(snapshot, timestamp):
        seen.append(snapshot.sequence)
        if len(seen) > 1 or not snapshot.asks:
            return []
        # make_order() fills in the id the engine requires.
        return [session.make_order(
            "buy", "limit", 25,
            ob.from_basis_points(snapshot.asks[0].price) - 0.10)]

    source = session.set_strategy(strategy)
    session.limit("sell", 100, 150.00)

    assert seen
    assert source.total_orders_submitted() == 1
    assert session.best_bid() == 149.90


def test_validate_answers_without_submitting(market):
    order = market.make_order("buy", "fok", 500, 150.50)

    verdict = market.validate(order)
    assert not verdict.valid
    assert "shortfall" in verdict.reason
    # Nothing was submitted.
    assert market.engine.orders_processed() == 2   # just the two seed orders


def test_reset_clears_the_event_log_and_statistics(market):
    market.market("buy", 50)
    assert market.fills

    market.reset()

    assert market.fills == []
    assert market.engine.orders_processed() == 0
    assert market.engine.heap_size() == 0
    # The book is deliberately untouched — see Session.reset().
    assert market.best_bid() == 149.50


def test_unknown_side_or_type_is_a_value_error(session):
    with pytest.raises(ValueError, match="side"):
        session.submit("sideways", "limit", 10, 150.00)
    with pytest.raises(ValueError, match="order type"):
        session.submit("buy", "iceberg", 10, 150.00)


def test_repr_is_informative(market):
    text = repr(market)
    assert "AAPL" in text and "149.5" in text
