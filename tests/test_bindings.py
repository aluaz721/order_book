"""
Smoke tests for the order_book Python bindings.

These exercise the compiled extension directly (not the C++ engine's own
GoogleTest suite, which lives under cpp/tests/) — the goal is to catch
regressions in the pybind11 binding layer itself: signatures, ownership
transfer (unique_ptr moves for matcher/book/source), and callback wiring.
"""

import struct

import pytest

import order_book as ob


# ── Order / price utils ───────────────────────────────────────────────────────

def test_price_utils_round_trip():
    bp = ob.to_basis_points(150.05)
    assert bp == 1500500
    assert ob.from_basis_points(bp) == pytest.approx(150.05)
    assert ob.format_price(bp) == "150.0500"


def test_order_construction_and_repr():
    order = ob.Order(
        id=1, symbol="AAPL", side=ob.Side.BID, type=ob.OrderType.LIMIT,
        price=1_500_000, quantity=100, timestamp=42,
    )
    assert order.id == 1
    assert order.side == ob.Side.BID
    assert order.quantity_remaining == 100
    assert order.orig_quantity == 100
    assert order.status == ob.OrderStatus.NEW
    assert "AAPL" in repr(order)


def test_opposite_side():
    assert ob.opposite(ob.Side.BID) == ob.Side.ASK
    assert ob.opposite(ob.Side.ASK) == ob.Side.BID


# ── TreeOrderBook ──────────────────────────────────────────────────────────────

def test_tree_order_book_matches_crossing_orders():
    fills = []
    cbs = ob.OrderBookCallbacks()
    cbs.on_fill = lambda f: fills.append(f)

    book = ob.TreeOrderBook("AAPL", ob.FIFOMatcher(), cbs)

    book.add(ob.Order(id=1, symbol="AAPL", side=ob.Side.BID, type=ob.OrderType.LIMIT,
                       price=1_500_000, quantity=100, timestamp=1))
    book.add(ob.Order(id=2, symbol="AAPL", side=ob.Side.ASK, type=ob.OrderType.LIMIT,
                       price=1_500_000, quantity=40, timestamp=2))

    assert len(fills) == 1
    assert fills[0].fill_quantity == 40
    assert book.best_bid() == 1_500_000
    assert book.best_ask() is None
    assert book.total_bid_qty() == 60

    book.cancel(1)
    assert not book.has_order(1)
    assert book.best_bid() is None


# ── HistoricalReplayer + SimulationEngine ────────────────────────────────────

def _u16(v): return struct.pack(">H", v)
def _u32(v): return struct.pack(">I", v)
def _u48(v): return v.to_bytes(6, "big")
def _u64(v): return struct.pack(">Q", v)
def _stock(s): return s.encode().ljust(8)


def _framed(payload: bytes) -> bytes:
    return _u16(len(payload)) + payload


def _itch_add(ref, side, shares, symbol, price_bp, ts, locate=1) -> bytes:
    return (b"A" + _u16(locate) + _u16(0) + _u48(ts) + _u64(ref) +
            side.encode() + _u32(shares) + _stock(symbol) + _u32(price_bp))


def _itch_execute(ref, shares, ts, locate=1) -> bytes:
    return b"E" + _u16(locate) + _u16(0) + _u48(ts) + _u64(ref) + _u32(shares) + _u64(0)


def test_historical_replayer_end_to_end(tmp_path):
    itch_file = (
        _framed(_itch_add(1, "B", 100, "AAPL", 1_500_000, 1_000)) +
        _framed(_itch_add(2, "S", 40, "AAPL", 1_500_500, 2_000)) +
        _framed(_itch_execute(1, 30, 3_000))
    )
    path = tmp_path / "sample.itch"
    path.write_bytes(itch_file)

    fills = []
    config = ob.SimulationConfig()
    config.symbol = "AAPL"

    engine = ob.SimulationEngine(config, ob.TreeOrderBook("AAPL", ob.FIFOMatcher()))
    engine.set_fill_callback(lambda f: fills.append(f))

    replay_config = ob.HistoricalReplayerConfig()
    replay_config.path = str(path)
    engine.add_source(ob.HistoricalReplayer(replay_config))

    engine.run()

    replayer = engine.historical_replayer()
    assert replayer.messages_parsed() == 3
    assert replayer.orders_replayed() == 3
    assert replayer.exhausted()

    assert len(fills) == 1
    assert fills[0].passive_order_id == 1
    assert fills[0].fill_quantity == 30

    book = engine.book()
    assert book.best_bid() == 1_500_000
    assert book.total_bid_qty() == 70  # 100 - 30
    assert book.has_order(2)  # the ask never crossed


def test_historical_replayer_missing_file_raises(tmp_path):
    replay_config = ob.HistoricalReplayerConfig()
    replay_config.path = str(tmp_path / "does_not_exist.itch")

    with pytest.raises(RuntimeError):
        ob.HistoricalReplayer(replay_config)
