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


# ── InteractiveSource / StopOrderManager / StrategySource ────────────────────
#
# These three were implemented in C++ but unbound, which left STOP and
# STOP_LIMIT unreachable from Python: they are in the OrderType enum, but
# TreeOrderBook.add() rejects them and only SimulationEngine.dispatch() routes
# them to the StopOrderManager. The tests below cover the bound surface that
# closes that gap.

def _engine(symbol="AAPL", order_id_start=1_000_000):
    config = ob.SimulationConfig()
    config.symbol = symbol
    config.order_id_start = order_id_start
    return ob.SimulationEngine(
        config, ob.TreeOrderBook(symbol, ob.FIFOMatcher()))


def test_stop_order_rejected_by_the_book_directly():
    # The reason the stop path has to exist: the book itself refuses stops.
    book = ob.TreeOrderBook("AAPL", ob.FIFOMatcher())
    with pytest.raises(RuntimeError, match="StopOrderManager"):
        book.add(ob.Order(id=1, symbol="AAPL", side=ob.Side.ASK,
                          type=ob.OrderType.STOP, price=1_490_000,
                          quantity=50, timestamp=1))


def test_interactive_source_queues_and_drains():
    engine = _engine()
    engine.add_source(ob.InteractiveSource())
    ui = engine.interactive_source()

    assert ui is not None
    assert ui.is_empty()
    assert not ui.exhausted()        # a live source is never exhausted

    ui.submit(ob.Order(id=1, symbol="AAPL", side=ob.Side.BID,
                       type=ob.OrderType.LIMIT, price=1_500_000,
                       quantity=100, timestamp=1))
    assert ui.pending_count() == 1

    engine.run()
    assert ui.pending_count() == 0
    assert engine.book().total_bid_qty() == 100

    ui.submit_cancel(1, "AAPL")
    engine.run()
    assert not engine.book().has_order(1)


def test_interactive_source_assigns_missing_order_ids():
    engine = _engine()
    engine.add_source(ob.InteractiveSource())
    acks = []
    engine.set_ack_callback(acks.append)

    engine.interactive_source().submit(
        ob.Order(id=0, symbol="AAPL", side=ob.Side.BID,
                 type=ob.OrderType.LIMIT, price=1_500_000, quantity=10))
    engine.run()

    assert engine.orders_rejected() == 0
    assert len(acks) == 1 and acks[0].order_id != 0


def test_stop_order_triggers_through_the_engine():
    engine = _engine()
    engine.add_source(ob.InteractiveSource())
    ui = engine.interactive_source()

    fills, triggers, acks = [], [], []
    engine.set_fill_callback(fills.append)
    engine.set_trigger_callback(triggers.append)
    engine.set_ack_callback(acks.append)

    def submit(order):
        ui.submit(order)
        engine.run()

    # Two-sided market, then a sell stop below it.
    submit(ob.Order(id=1, symbol="AAPL", side=ob.Side.BID,
                    type=ob.OrderType.LIMIT, price=1_495_000, quantity=100))
    submit(ob.Order(id=2, symbol="AAPL", side=ob.Side.ASK,
                    type=ob.OrderType.LIMIT, price=1_505_000, quantity=100))
    submit(ob.Order(id=3, symbol="AAPL", side=ob.Side.ASK,
                    type=ob.OrderType.STOP, price=1_490_000, quantity=50))

    stops = engine.stop_manager()
    assert stops.pending_stop_count() == 1
    assert stops.has_stop(3)
    assert stops.nearest_stop_sell() == 1_490_000
    assert acks[-1].status == ob.OrderStatus.PENDING_TRIGGER
    # A pending stop is invisible in the book.
    assert engine.book().total_ask_qty() == 100
    assert not engine.book().has_order(3)

    # Sweep down through the stop price: fills at 149.50 then 148.50, the
    # second of which fires the stop.
    submit(ob.Order(id=4, symbol="AAPL", side=ob.Side.BID,
                    type=ob.OrderType.LIMIT, price=1_485_000, quantity=200))
    submit(ob.Order(id=5, symbol="AAPL", side=ob.Side.ASK,
                    type=ob.OrderType.LIMIT, price=1_480_000, quantity=150))

    assert len(triggers) == 1
    assert triggers[0].stop_order_id == 3
    assert triggers[0].converted_to == ob.OrderType.MARKET
    assert triggers[0].triggered_order_id >= 1_000_000   # engine's id counter
    assert engine.stops_triggered() == 1
    assert stops.pending_stop_count() == 0

    # The converted MARKET sell traded against the remaining 148.50 bid.
    triggered = [f for f in fills
                 if f.aggressive_order_id == triggers[0].triggered_order_id]
    assert len(triggered) == 1
    assert triggered[0].fill_quantity == 50
    assert triggered[0].fill_price == 1_485_000


def test_pending_stop_can_be_cancelled_through_the_stop_manager():
    engine = _engine()
    engine.add_source(ob.InteractiveSource())
    cancels = []
    engine.set_cancel_callback(cancels.append)

    engine.interactive_source().submit(
        ob.Order(id=1, symbol="AAPL", side=ob.Side.ASK,
                 type=ob.OrderType.STOP, price=1_490_000, quantity=50))
    engine.run()
    assert engine.stop_manager().pending_stop_count() == 1

    engine.stop_manager().cancel(1, 0)
    assert engine.stop_manager().pending_stop_count() == 0
    assert not engine.stop_manager().has_stop(1)
    assert len(cancels) == 1
    assert cancels[0].reason == ob.CancelReason.STOP_CANCELLED


def test_stop_manager_can_drive_a_bare_book():
    # Without an engine, the caller owns the record/drain contract: record the
    # trade from on_fill (the book is mid-match there), drain once add()
    # returns.
    triggers = []
    fills = []

    cbs = ob.OrderBookCallbacks()
    book = ob.TreeOrderBook("AAPL", ob.FIFOMatcher(), cbs)
    stops = ob.StopOrderManager(book, on_trigger=triggers.append)

    def on_fill(fill):
        fills.append(fill)
        stops.record_trade(fill.fill_price, fill.timestamp)

    cbs.on_fill = on_fill
    book.set_callbacks(cbs)

    stops.submit(ob.Order(id=1, symbol="AAPL", side=ob.Side.ASK,
                          type=ob.OrderType.STOP, price=1_490_000, quantity=50))
    assert stops.pending_stop_count() == 1

    # Resting bids at 149.00 and 148.00; a sell into the first trades at
    # 149.00, which is at the stop price.
    book.add(ob.Order(id=2, symbol="AAPL", side=ob.Side.BID,
                      type=ob.OrderType.LIMIT, price=1_490_000, quantity=100))
    book.add(ob.Order(id=3, symbol="AAPL", side=ob.Side.BID,
                      type=ob.OrderType.LIMIT, price=1_480_000, quantity=100))
    book.add(ob.Order(id=4, symbol="AAPL", side=ob.Side.ASK,
                      type=ob.OrderType.LIMIT, price=1_490_000, quantity=100))
    stops.drain_triggers()

    assert len(triggers) == 1
    assert stops.pending_stop_count() == 0
    # The stop's converted MARKET sell took the 148.00 bid.
    assert fills[-1].fill_price == 1_480_000
    assert fills[-1].fill_quantity == 50
    # The book survived the cascade with consistent bookkeeping.
    assert book.total_bid_qty() == 50
    assert book.bid_depth() == 1


def test_strategy_source_quotes_from_a_python_callback():
    engine = _engine()
    engine.add_source(ob.InteractiveSource())

    invocations = []

    def strategy(snapshot, timestamp):
        invocations.append(timestamp)
        if len(invocations) > 1 or not snapshot.asks:
            return []
        # Join the book one cent inside the best ask.
        return [ob.Order(id=7_001, symbol="AAPL", side=ob.Side.BID,
                         type=ob.OrderType.LIMIT,
                         price=snapshot.asks[0].price - 1_000,
                         quantity=25, timestamp=timestamp)]

    engine.add_source(ob.StrategySource(strategy))
    source = engine.strategy_source()
    assert source is not None

    engine.interactive_source().submit(
        ob.Order(id=1, symbol="AAPL", side=ob.Side.ASK,
                 type=ob.OrderType.LIMIT, price=1_500_000, quantity=100))
    engine.run()

    assert invocations                         # the callback ran
    assert source.total_orders_submitted() == 1
    # Buffered during the ask's book update, dispatched on a later tick.
    assert engine.book().has_order(7_001)
    assert engine.book().best_bid() == 1_499_000


def test_engine_source_heap_stays_bounded():
    engine = _engine()
    engine.add_source(ob.InteractiveSource())
    engine.add_source(ob.StrategySource(lambda snapshot, ts: []))

    ui = engine.interactive_source()
    for i in range(1, 201):
        ui.submit(ob.Order(id=i, symbol="AAPL", side=ob.Side.BID,
                           type=ob.OrderType.LIMIT, price=1_400_000,
                           quantity=1, timestamp=i))
    engine.run()

    assert engine.orders_processed() == 200
    assert engine.heap_size() <= 5


# ── Validators ───────────────────────────────────────────────────────────────

def test_validators_report_the_same_verdicts_the_engine_uses():
    book = ob.TreeOrderBook("AAPL", ob.FIFOMatcher())
    book.add(ob.Order(id=1, symbol="AAPL", side=ob.Side.ASK,
                      type=ob.OrderType.LIMIT, price=1_500_000, quantity=40))

    ok = ob.validators.validate_fields(
        ob.Order(id=2, symbol="AAPL", side=ob.Side.BID,
                 type=ob.OrderType.LIMIT, price=1_500_000, quantity=10))
    assert ok.valid and bool(ok) and ok.reason == ""

    no_id = ob.validators.validate_fields(
        ob.Order(id=0, symbol="AAPL", side=ob.Side.BID,
                 type=ob.OrderType.LIMIT, price=1_500_000, quantity=10))
    assert not no_id.valid and "id" in no_id.reason

    # FOK for more than the book holds.
    fok = ob.validators.validate_fok(
        ob.Order(id=3, symbol="AAPL", side=ob.Side.BID,
                 type=ob.OrderType.FILL_OR_KILL, price=1_500_000,
                 quantity=100), book)
    assert not fok.valid and "shortfall" in fok.reason

    # A sell stop has to sit below the market, and there is no bid here, so
    # the last trade price is the reference — still none, so it passes.
    stop = ob.validators.validate(
        ob.Order(id=4, symbol="AAPL", side=ob.Side.ASK,
                 type=ob.OrderType.STOP, price=1_490_000, quantity=10), book)
    assert stop.valid


# ── Threading / GIL ───────────────────────────────────────────────────────────

def test_engine_run_releases_the_gil(tmp_path):
    """A paced replay must not freeze other Python threads.

    SimulationEngine.run() blocks for the whole simulation and, with a paced
    HistoricalReplayer, sleeps between events. Without
    py::call_guard<py::gil_scoped_release> on the binding it would hold the
    GIL throughout — under uvicorn that stops the asyncio loop from
    servicing WebSockets or HTTP for the entire replay.

    Asserts a lower bound on the watcher's progress only: an upper bound
    would be measuring the machine's scheduler rather than the binding.
    """
    import threading
    import time

    path = tmp_path / "paced.itch"
    path.write_bytes(b"".join(
        _framed(_itch_add(100 + i, "B", 100, "AAPL", 1_500_000 + i,
                          1_000_000_000 + i * 1_000_000))
        for i in range(10)))

    config = ob.HistoricalReplayerConfig()
    config.path = str(path)
    config.speed = ob.ReplaySpeed.FIXED_NS
    config.fixed_ns = 5_000_000            # 5ms per event -> ~50ms total

    sim_config = ob.SimulationConfig()
    sim_config.symbol = "AAPL"

    engine = ob.SimulationEngine(
        sim_config, ob.TreeOrderBook("AAPL", ob.FIFOMatcher()))
    engine.add_source(ob.HistoricalReplayer(config))

    ticks = 0
    stop = threading.Event()

    def watcher():
        nonlocal ticks
        while not stop.is_set():
            ticks += 1
            time.sleep(0.001)

    worker = threading.Thread(target=watcher, daemon=True)
    worker.start()

    start = time.perf_counter()
    engine.run()
    elapsed = time.perf_counter() - start

    stop.set()
    worker.join(timeout=1)

    assert engine.orders_processed() == 10
    # Pacing actually applied (10 events x 5ms), so the GIL was genuinely
    # held-or-released across a meaningful window rather than a few micros.
    assert elapsed > 0.03, f"replay was not paced (took {elapsed:.3f}s)"
    # The watcher kept getting scheduled while run() was sleeping.
    assert ticks > 3, f"GIL held during run(): watcher ran only {ticks} times"

