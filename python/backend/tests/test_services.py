"""Integration tests for EngineBridge against the real C++ engine.

These exercise the compiled `order_book` extension, not a Python stand-in, so
a failure here means either the bridge's translation layer or the engine
itself is wrong.
"""

import order_book as ob
import pytest

from fastapi import HTTPException

from app.models import CancelRequest, OrderRequest
from app.services import EngineBridge


def order(side="buy", price=100.0, quantity=10, order_type="limit", symbol="AAPL"):
    return OrderRequest(side=side, price=price, quantity=quantity,
                        order_type=order_type, symbol=symbol)


@pytest.fixture
def bridge():
    return EngineBridge()


# ── Submission basics ─────────────────────────────────────────────────────────

def test_submit_order_works_without_running_event_loop(bridge):
    response = bridge.submit_order(order(side="buy", price=100.0, quantity=2))

    assert response.accepted is True
    assert response.order_id == 1
    assert response.status == "accepted"


def test_resting_limit_order_appears_in_engine_snapshot(bridge):
    bridge.submit_order(order(side="buy", price=100.25, quantity=7))

    snap = bridge._build_snapshot()

    assert len(snap.bids) == 1
    level = snap.bids[0]
    assert level.price == 100.25          # survives the basis-point round trip
    assert level.quantity == 7
    assert level.order_count == 1
    assert level.order_ids == [1]
    assert snap.best_bid == 100.25
    assert snap.best_ask is None
    assert snap.resting_orders[0].quantity == 7


def test_prices_round_trip_without_float_drift(bridge):
    # The engine stores int64 basis points; these are the prices most likely
    # to expose a bad conversion.
    for price in (100.01, 100.05, 0.0001, 1234.5678):
        b = EngineBridge()
        b.submit_order(order(side="sell", price=price, quantity=1))
        assert b._build_snapshot().asks[0].price == price


# ── Matching semantics ────────────────────────────────────────────────────────

def test_fifo_time_priority_across_two_orders_at_same_price(bridge):
    bridge.submit_order(order(side="buy", price=100.0, quantity=100))  # id 1
    bridge.submit_order(order(side="buy", price=100.0, quantity=50))   # id 2

    # Aggressive sell for 120 takes all of id 1, then 20 of id 2.
    response = bridge.submit_order(order(side="sell", price=100.0, quantity=120))

    assert response.status == "filled"
    fills = bridge.get_fills()
    assert [(f.passive_order_id, f.quantity) for f in fills] == [(1, 100), (2, 20)]
    assert all(f.price == 100.0 and f.side == "sell" for f in fills)

    # id 1 is gone, id 2 is still resting with 30 left.
    resting = {r.order_id: r for r in bridge._build_snapshot().resting_orders}
    assert 1 not in resting
    assert resting[2].quantity == 30
    assert resting[2].orig_quantity == 50


def test_market_order_sweeps_multiple_levels(bridge):
    bridge.submit_order(order(side="sell", price=100.0, quantity=40))
    bridge.submit_order(order(side="sell", price=101.0, quantity=40))

    response = bridge.submit_order(order(side="buy", order_type="market",
                                         price=1.0, quantity=70))

    assert response.accepted is True
    assert response.status == "filled"
    assert [f.price for f in bridge.get_fills()] == [100.0, 101.0]


def test_market_order_without_liquidity_is_rejected(bridge):
    response = bridge.submit_order(order(side="buy", order_type="market",
                                         price=1.0, quantity=5))

    assert response.accepted is False
    assert response.status == "cancelled"
    assert "liquidity" in response.message
    assert bridge.get_fills() == []


def test_fok_that_cannot_fill_produces_no_fills(bridge):
    bridge.submit_order(order(side="sell", price=100.0, quantity=10))

    response = bridge.submit_order(order(side="buy", order_type="fok",
                                         price=100.0, quantity=999))

    # The engine pre-checks FOK, so it must not leak partial fills.
    assert response.accepted is False
    assert response.status == "cancelled"
    assert bridge.get_fills() == []
    assert bridge._build_snapshot().asks[0].quantity == 10  # untouched


def test_fok_that_can_fill_executes_completely(bridge):
    bridge.submit_order(order(side="sell", price=100.0, quantity=10))

    response = bridge.submit_order(order(side="buy", order_type="fok",
                                         price=100.0, quantity=10))

    assert response.accepted is True
    assert response.status == "filled"
    assert sum(f.quantity for f in bridge.get_fills()) == 10


def test_ioc_fills_what_it_can_and_cancels_the_rest(bridge):
    bridge.submit_order(order(side="sell", price=100.0, quantity=10))

    response = bridge.submit_order(order(side="buy", order_type="ioc",
                                         price=100.0, quantity=25))

    assert response.accepted is True
    assert response.status == "partially_filled"
    assert "10/25" in response.message
    assert sum(f.quantity for f in bridge.get_fills()) == 10
    # The IOC remainder never rests.
    assert bridge._build_snapshot().bids == []


# ── Cancellation ──────────────────────────────────────────────────────────────

def test_cancel_removes_order_from_the_book(bridge):
    bridge.submit_order(order(side="buy", price=100.0, quantity=10))

    response = bridge.cancel_order(CancelRequest(order_id=1))

    assert response.accepted is True
    snap = bridge._build_snapshot()
    assert snap.bids == []
    assert snap.resting_orders == []


def test_cancel_unknown_order_is_reported_not_silently_accepted(bridge):
    response = bridge.cancel_order(CancelRequest(order_id=4242))

    assert response.accepted is False
    assert "not found" in response.message


def test_cancel_filled_order_is_reported_as_missing(bridge):
    bridge.submit_order(order(side="buy", price=100.0, quantity=10))
    bridge.submit_order(order(side="sell", price=100.0, quantity=10))

    response = bridge.cancel_order(CancelRequest(order_id=1))

    assert response.accepted is False


# ── Projection integrity ──────────────────────────────────────────────────────

def test_projection_never_diverges_from_engine_levels(bridge):
    # Mix of resting, partially filled, cancelled and swept orders.
    bridge.submit_order(order(side="buy", price=100.0, quantity=100))
    bridge.submit_order(order(side="buy", price=100.0, quantity=50))
    bridge.submit_order(order(side="buy", price=99.5, quantity=25))
    bridge.submit_order(order(side="sell", price=101.0, quantity=60))
    bridge.submit_order(order(side="sell", price=100.0, quantity=120))
    bridge.cancel_order(CancelRequest(order_id=3))

    snap = bridge._build_snapshot()

    # Every level's engine quantity equals the sum of the projection's
    # per-order remainders at that level, and no repairs were needed.
    for side_levels, side in ((snap.bids, "buy"), (snap.asks, "sell")):
        for level in side_levels:
            projected = sum(r.quantity for r in snap.resting_orders
                            if r.side == side and r.price == level.price)
            assert projected == level.quantity
            assert len(level.order_ids) == level.order_count

    assert bridge.diagnostics()["projection_repairs"] == 0


def test_engine_is_the_source_of_truth_for_sequence(bridge):
    bridge.submit_order(order())
    diag = bridge.diagnostics()

    assert diag["engine_sequence"] == bridge._build_snapshot().sequence
    assert diag["engine_updates"] > 0


def test_bridge_uses_the_compiled_engine_not_a_python_fallback(bridge):
    # Guards against the bridge regressing to an in-Python matcher.
    assert isinstance(bridge._book, ob.TreeOrderBook)


# ── MARKET_LIMIT ──────────────────────────────────────────────────────────────

def test_market_limit_rests_its_remainder_at_the_traded_price(bridge):
    bridge.submit_order(order(side="sell", price=100.00, quantity=40))

    # The price submitted is deliberately far from the market: matching
    # ignores it (MARKET_LIMIT always crosses) and the remainder rests at the
    # price that actually traded.
    response = bridge.submit_order(
        order(side="buy", price=120.00, quantity=100, order_type="market_limit"))

    assert response.accepted is True
    snap = bridge._build_snapshot()
    assert snap.best_bid == 100.00          # the traded price, not 120.00
    assert snap.bids[0].quantity == 60


# ── Stop orders ───────────────────────────────────────────────────────────────

def stop(side="sell", stop_price=99.00, quantity=10, limit_price=None,
         order_type="stop", price=100.0):
    return OrderRequest(side=side, price=price, quantity=quantity,
                        order_type=order_type, symbol="AAPL",
                        stop_price=stop_price, limit_price=limit_price)


@pytest.fixture
def market(bridge):
    """A two-sided book: 100 bid at 99.50, 100 offered at 100.50."""
    bridge.submit_order(order(side="buy", price=99.50, quantity=100))
    bridge.submit_order(order(side="sell", price=100.50, quantity=100))
    return bridge


def test_accepted_stop_is_held_off_the_book(market):
    response = market.submit_order(stop(side="sell", stop_price=99.00, quantity=50))

    assert response.accepted is True
    assert response.status == "pending_trigger"
    assert "99.00" in response.message

    snap = market._build_snapshot()
    # Invisible to depth, quantity and the touch — the whole point of holding
    # stops outside the book.
    assert len(snap.asks) == 1
    assert snap.asks[0].quantity == 100
    assert snap.best_ask == 100.50
    assert [o.order_id for o in snap.resting_orders] == [1, 2]

    # Visible in its own list instead.
    assert len(snap.pending_stops) == 1
    pending = snap.pending_stops[0]
    assert pending.order_id == response.order_id
    assert pending.order_type == "stop"
    assert pending.stop_price == 99.00
    assert pending.limit_price is None
    assert pending.quantity == 50
    assert market.diagnostics()["pending_stops"] == 1


def test_stop_that_would_trigger_immediately_is_rejected(market):
    # A sell stop at 100.00 sits above the 99.50 bid, so a trade at the bid
    # would fire it at once.
    response = market.submit_order(stop(side="sell", stop_price=100.00))

    assert response.accepted is False
    assert response.status == "rejected"
    assert "below the current market" in response.message
    assert market.diagnostics()["pending_stops"] == 0
    assert market._build_snapshot().pending_stops == []


def test_stop_buy_must_sit_above_the_market(market):
    assert market.submit_order(stop(side="buy", stop_price=99.00)).accepted is False
    assert market.submit_order(stop(side="buy", stop_price=101.00)).accepted is True


def test_stop_fires_when_a_trade_crosses_it_and_the_response_says_so(market):
    pending = market.submit_order(stop(side="sell", stop_price=99.00, quantity=50))

    # Sell through the book: fills at 99.50, then at 98.50 — the second is
    # at-or-below 99.00 and fires the stop.
    market.submit_order(order(side="buy", price=98.50, quantity=200))
    swept = market.submit_order(order(side="sell", price=98.00, quantity=150))

    assert swept.accepted is True
    assert f"Triggered pending stop {pending.order_id}" in swept.message
    assert market.diagnostics()["stops_triggered"] == 1
    assert market.diagnostics()["pending_stops"] == 0
    assert market._build_snapshot().pending_stops == []

    # The converted MARKET sell traded against the remaining 98.50 bid, on
    # top of the 150 the sweep itself did.
    assert sum(f.quantity for f in market.get_fills()) == 200


def test_triggered_order_takes_an_id_from_the_same_counter(market):
    submitted = {1, 2}   # the two orders the `market` fixture posted
    submitted.add(market.submit_order(
        stop(side="sell", stop_price=99.00, quantity=50)).order_id)
    submitted.add(market.submit_order(
        order(side="buy", price=98.50, quantity=200)).order_id)
    submitted.add(market.submit_order(
        order(side="sell", price=98.00, quantity=150)).order_id)

    aggressors = {f.aggressive_order_id for f in market.get_fills()}
    converted = aggressors - submitted

    # The order the stop converted into is one the client never submitted,
    # numbered from the bridge's own counter rather than colliding with it.
    assert len(converted) == 1
    assert converted.pop() == max(submitted) + 1


def test_stop_limit_converts_to_a_resting_limit_at_its_limit_price(market):
    # Sell-stop convention: limit_price >= stop_price.
    market.submit_order(stop(side="sell", stop_price=99.00, limit_price=99.25,
                             quantity=25, order_type="stop_limit"))

    market.submit_order(order(side="buy", price=98.50, quantity=200))
    swept = market.submit_order(order(side="sell", price=98.00, quantity=150))

    assert "Triggered pending stop" in swept.message

    snap = market._build_snapshot()
    # Converted to a LIMIT sell at 99.25, which doesn't cross the remaining
    # 98.50 bid — so unlike a STOP, it rests.
    assert snap.best_ask == 99.25
    resting = [o for o in snap.resting_orders if o.price == 99.25]
    assert len(resting) == 1
    # Labelled as what it became, not what it was.
    assert resting[0].order_type == "limit"
    assert resting[0].quantity == 25


def test_stop_limit_with_its_limit_on_the_wrong_side_is_rejected(market):
    response = market.submit_order(
        stop(side="buy", stop_price=101.00, limit_price=102.00,
             order_type="stop_limit"))

    assert response.accepted is False
    assert "limit_price" in response.message
    assert market.diagnostics()["pending_stops"] == 0


def test_stop_limit_without_a_limit_price_is_a_422(market):
    with pytest.raises(HTTPException) as excinfo:
        market.submit_order(stop(side="sell", stop_price=99.00,
                                 order_type="stop_limit"))

    assert excinfo.value.status_code == 422
    assert "limit_price" in str(excinfo.value.detail)


def test_stop_price_falls_back_to_the_price_field(market):
    # A client that doesn't know about stop_price can post a stop the same
    # shape as a limit order.
    response = market.submit_order(
        OrderRequest(side="sell", price=99.00, quantity=10, order_type="stop",
                     symbol="AAPL"))

    assert response.status == "pending_trigger"
    assert market._build_snapshot().pending_stops[0].stop_price == 99.00


# ── Cancelling a pending stop ─────────────────────────────────────────────────

def test_pending_stop_is_cancelled_through_the_same_endpoint(market):
    pending = market.submit_order(stop(side="sell", stop_price=99.00, quantity=50))

    response = market.cancel_order(CancelRequest(order_id=pending.order_id))

    assert response.accepted is True
    assert "stop" in response.message.lower()
    assert market.diagnostics()["pending_stops"] == 0
    assert market._build_snapshot().pending_stops == []


def test_cancelled_stop_does_not_fire_later(market):
    pending = market.submit_order(stop(side="sell", stop_price=99.00, quantity=50))
    market.cancel_order(CancelRequest(order_id=pending.order_id))

    market.submit_order(order(side="buy", price=98.50, quantity=200))
    swept = market.submit_order(order(side="sell", price=98.00, quantity=150))

    assert "Triggered" not in swept.message
    assert market.diagnostics()["stops_triggered"] == 0


def test_cancelling_an_unknown_id_still_reports_not_found(market):
    assert market.cancel_order(CancelRequest(order_id=999)).accepted is False


# ── The projection survives a trigger ─────────────────────────────────────────

def test_projection_does_not_drift_through_a_stop_cascade(market):
    # Two stops at different prices, the second reachable only via the fill
    # the first one's converted order produces.
    market.submit_order(stop(side="sell", stop_price=99.00, quantity=100))
    market.submit_order(stop(side="sell", stop_price=98.50, quantity=100))

    market.submit_order(order(side="buy", price=98.50, quantity=100))
    market.submit_order(order(side="buy", price=97.00, quantity=200))
    market.submit_order(order(side="sell", price=97.00, quantity=150))

    snap = market._build_snapshot()
    diagnostics = market.diagnostics()

    # Whatever fired, the projection must agree with the engine: every
    # resting order it reports is one the book confirms, and nothing had to
    # be repaired.
    assert diagnostics["projection_repairs"] == 0
    assert diagnostics["pending_stops"] == len(snap.pending_stops)
    for resting in snap.resting_orders:
        assert market._book.has_order(resting.order_id)
    engine_qty = market._book.total_bid_qty() + market._book.total_ask_qty()
    assert sum(o.quantity for o in snap.resting_orders) == engine_qty
