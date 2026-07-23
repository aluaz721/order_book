"""
Tests for order_book.features.microstructure — pure functions over
BookSnapshot/BookLevel/FillEvent, independent of any live engine state.
"""

import pytest

import order_book as ob
from order_book.features import microstructure as ms


@pytest.fixture
def two_sided_book():
    book = ob.TreeOrderBook("AAPL", ob.FIFOMatcher())
    book.add(ob.Order(id=1, symbol="AAPL", side=ob.Side.BID, type=ob.OrderType.LIMIT,
                       price=1_500_000, quantity=100, timestamp=1))
    book.add(ob.Order(id=2, symbol="AAPL", side=ob.Side.ASK, type=ob.OrderType.LIMIT,
                       price=1_500_500, quantity=40, timestamp=2))
    return book


def test_mid_spread_relative_spread(two_sided_book):
    snap = two_sided_book.snapshot()
    assert ms.mid_price(snap) == pytest.approx(1_500_250.0)
    assert ms.spread(snap) == 500
    assert ms.relative_spread(snap) == pytest.approx(500 / 1_500_250.0)


def test_micro_price_weights_toward_thinner_side(two_sided_book):
    snap = two_sided_book.snapshot()
    # ask side (40) is thinner than bid side (100) -> microprice pulled
    # toward the ask relative to the simple mid.
    micro = ms.micro_price(snap)
    mid = ms.mid_price(snap)
    assert micro > mid


def test_order_book_imbalance(two_sided_book):
    snap = two_sided_book.snapshot()
    # (100 - 40) / (100 + 40)
    assert ms.order_book_imbalance(snap) == pytest.approx(60 / 140)


def test_depth_respects_max_levels():
    book = ob.TreeOrderBook("AAPL", ob.FIFOMatcher())
    book.add(ob.Order(id=1, symbol="AAPL", side=ob.Side.BID, type=ob.OrderType.LIMIT,
                       price=1_500_000, quantity=10, timestamp=1))
    book.add(ob.Order(id=2, symbol="AAPL", side=ob.Side.BID, type=ob.OrderType.LIMIT,
                       price=1_499_000, quantity=20, timestamp=2))
    snap = book.snapshot()
    assert ms.depth(snap.bids, max_levels=1) == 10
    assert ms.depth(snap.bids) == 30


def test_volume_weighted_price_and_price_impact(two_sided_book):
    snap = two_sided_book.snapshot()
    vwap = ms.volume_weighted_price(snap.asks, 30)
    assert vwap == pytest.approx(1_500_500.0)  # fully within the single level

    assert ms.volume_weighted_price(snap.asks, 999) is None  # not enough liquidity

    impact = ms.price_impact(snap, ob.Side.BID, 30)
    assert impact == pytest.approx(0.0)  # doesn't walk past the best level


def test_effective_spread_from_a_real_fill():
    fills = []
    cbs = ob.OrderBookCallbacks()
    cbs.on_fill = lambda f: fills.append(f)
    book = ob.TreeOrderBook("AAPL", ob.FIFOMatcher(), cbs)

    book.add(ob.Order(id=1, symbol="AAPL", side=ob.Side.BID, type=ob.OrderType.LIMIT,
                       price=1_500_000, quantity=100, timestamp=1))
    book.add(ob.Order(id=2, symbol="AAPL", side=ob.Side.ASK, type=ob.OrderType.LIMIT,
                       price=1_500_500, quantity=40, timestamp=2))

    mid_before = ms.mid_price(book.snapshot())

    # aggressive buy crosses and fills against the resting ask at 1_500_500
    book.add(ob.Order(id=3, symbol="AAPL", side=ob.Side.BID, type=ob.OrderType.LIMIT,
                       price=1_500_500, quantity=10, timestamp=3))

    assert len(fills) == 1
    assert ms.effective_spread(fills[0], mid_before) == pytest.approx(
        2.0 * (fills[0].fill_price - mid_before)
    )


def test_empty_book_returns_none():
    book = ob.TreeOrderBook("AAPL", ob.FIFOMatcher())
    snap = book.snapshot()
    assert ms.mid_price(snap) is None
    assert ms.spread(snap) is None
    assert ms.relative_spread(snap) is None
    assert ms.micro_price(snap) is None
    assert ms.order_book_imbalance(snap) is None
