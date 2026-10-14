"""Integration tests for EngineBridge against the real C++ engine.

These exercise the compiled `order_book` extension, not a Python stand-in, so
a failure here means either the bridge's translation layer or the engine
itself is wrong.
"""

import order_book as ob
import pytest

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
