"""HTTP/WebSocket surface tests.

Covers the paths that only appear once FastAPI is driving the bridge — in
particular the WebSocket broadcast, which is scheduled from a worker thread
while the event loop runs on another.
"""

import pytest
from fastapi.testclient import TestClient

import app.main as main
from app.models import OrderRequest
from app.services import EngineBridge


@pytest.fixture
def client():
    # Fresh engine per test — `main.bridge` is module-level state.
    main.bridge = EngineBridge()
    with TestClient(main.app) as c:
        yield c


def test_health(client):
    assert client.get("/health").json() == {"status": "ok"}


def test_submit_and_read_back_fills(client):
    client.post("/orders", json={"side": "buy", "price": 100.0,
                                 "quantity": 10, "order_type": "limit",
                                 "symbol": "AAPL"})
    res = client.post("/orders", json={"side": "sell", "price": 100.0,
                                       "quantity": 4, "order_type": "limit",
                                       "symbol": "AAPL"})
    assert res.status_code == 200
    assert res.json()["accepted"] is True

    fills = client.get("/fills").json()
    assert len(fills) == 1
    assert fills[0]["quantity"] == 4
    assert fills[0]["price"] == 100.0
    assert fills[0]["passive_order_id"] == 1


def test_rejected_order_returns_accepted_false(client):
    res = client.post("/orders", json={"side": "buy", "price": 100.0,
                                       "quantity": 5, "order_type": "market",
                                       "symbol": "AAPL"})
    body = res.json()
    assert res.status_code == 200          # a rejection is not a transport error
    assert body["accepted"] is False
    assert body["status"] == "cancelled"


def test_cancel_endpoint(client):
    client.post("/orders", json={"side": "buy", "price": 100.0, "quantity": 10,
                                 "order_type": "limit", "symbol": "AAPL"})

    assert client.delete("/orders/1").json()["accepted"] is True
    assert client.delete("/orders/1").json()["accepted"] is False


def test_unknown_backtest_run_returns_404(client):
    assert client.get("/backtests/nope").status_code == 404


def test_backtest_reports_not_implemented(client):
    body = client.post("/backtests", json={}).json()
    assert body["status"] == "not_implemented"


def test_websocket_receives_updates_driven_from_a_background_thread(client):
    """A broadcast raised off the serving loop still reaches the client.

    This is the path a replay takes: the engine runs on its own thread, so
    `_schedule_broadcast` finds no running loop and must hand the coroutine
    to the loop that owns the WebSocket.

    Note this does NOT by itself prove the hand-off is correct — TestClient
    backs its WebSocket with an in-memory queue, so even the old
    `asyncio.run()` implementation (a second loop touching a socket it does
    not own) delivers the frame here. The cross-loop hazard only bites under
    a real uvicorn server. `test_broadcast_from_a_worker_thread_uses_the_
    serving_loop` below is the regression test for that; this one covers the
    ordinary expectation that an off-loop submission neither raises nor
    hangs.
    """
    import threading

    with client.websocket_connect("/ws/book") as ws:
        assert ws.receive_json()["type"] == "snapshot"

        error: list[BaseException] = []

        def submit_off_loop():
            try:
                main.bridge.submit_order(OrderRequest(
                    side="buy", price=101.0, quantity=7,
                    order_type="limit", symbol="AAPL"))
            except BaseException as exc:          # pragma: no cover
                error.append(exc)

        worker = threading.Thread(target=submit_off_loop)
        worker.start()
        worker.join(timeout=5)

        assert not error, f"submission from a worker thread raised {error}"
        assert not worker.is_alive(), "submission from a worker thread hung"

        # Must arrive because the broadcast was marshalled correctly — not
        # because connect()'s 5-second keep-alive eventually fired.
        update = ws.receive_json()
        assert update["book"]["bids"][0]["price"] == 101.0
        assert update["book"]["bids"][0]["quantity"] == 7


def test_bridge_binds_the_serving_loop_at_startup(client):
    """lifespan hands the bridge the loop uvicorn serves on."""
    assert main.bridge._loop is not None
    assert not main.bridge._loop.is_closed()


def test_broadcast_from_a_worker_thread_uses_the_serving_loop(client, monkeypatch):
    """Off-loop broadcasts must be marshalled, never run on a fresh loop.

    The regression this guards: `_schedule_broadcast` used to fall back to
    `asyncio.run(self._broadcast())` whenever it could not find a running
    loop, which is every call from a FastAPI threadpool worker and every
    call from a replay's engine thread. That builds a second event loop and
    awaits `send_text()` on WebSockets owned by uvicorn's loop.

    TestClient's in-memory socket hides the consequence, so assert on the
    mechanism instead: `asyncio.run` must not be reached, and the coroutine
    must be submitted to the bound loop.
    """
    import asyncio
    import threading

    def explode(*args, **kwargs):                       # pragma: no cover
        raise AssertionError(
            "_schedule_broadcast built a new event loop instead of "
            "marshalling onto the serving loop")

    monkeypatch.setattr(asyncio, "run", explode)

    submitted: list[object] = []
    real_submit = asyncio.run_coroutine_threadsafe

    def record(coro, loop):
        submitted.append(loop)
        return real_submit(coro, loop)

    monkeypatch.setattr(asyncio, "run_coroutine_threadsafe", record)

    with client.websocket_connect("/ws/book") as ws:
        ws.receive_json()
        serving_loop = main.bridge._loop

        worker = threading.Thread(target=lambda: main.bridge.submit_order(
            OrderRequest(side="buy", price=102.0, quantity=3,
                         order_type="limit", symbol="AAPL")))
        worker.start()
        worker.join(timeout=5)
        assert not worker.is_alive()

    assert submitted, "broadcast was never handed to the serving loop"
    assert all(loop is serving_loop for loop in submitted)


def test_websocket_pushes_snapshot_on_connect_and_after_an_order(client):
    with client.websocket_connect("/ws/book") as ws:
        initial = ws.receive_json()
        assert initial["type"] == "snapshot"
        assert initial["book"]["bids"] == []

        client.post("/orders", json={"side": "buy", "price": 100.0,
                                     "quantity": 10, "order_type": "limit",
                                     "symbol": "AAPL"})

        update = ws.receive_json()
        assert update["book"]["bids"][0]["price"] == 100.0
        assert update["book"]["bids"][0]["quantity"] == 10
        assert update["book"]["best_bid"] == 100.0


# ── Stop orders over HTTP ─────────────────────────────────────────────────────

def seed_market(client):
    """Two-sided book: 100 bid at 99.50, 100 offered at 100.50."""
    client.post("/orders", json={"side": "buy", "price": 99.50, "quantity": 100,
                                 "order_type": "limit", "symbol": "AAPL"})
    client.post("/orders", json={"side": "sell", "price": 100.50, "quantity": 100,
                                 "order_type": "limit", "symbol": "AAPL"})


def test_all_seven_order_types_are_accepted_by_the_schema(client):
    seed_market(client)

    bodies = [
        {"side": "sell", "price": 99.50, "quantity": 10, "order_type": "limit"},
        {"side": "buy", "price": 100.50, "quantity": 10, "order_type": "market"},
        {"side": "buy", "price": 120.00, "quantity": 10,
         "order_type": "market_limit"},
        {"side": "buy", "price": 100.50, "quantity": 10, "order_type": "ioc"},
        {"side": "buy", "price": 100.50, "quantity": 10, "order_type": "fok"},
        {"side": "sell", "price": 99.00, "quantity": 10, "order_type": "stop",
         "stop_price": 99.00},
        {"side": "sell", "price": 98.00, "quantity": 10,
         "order_type": "stop_limit", "stop_price": 98.00, "limit_price": 98.25},
    ]

    for body in bodies:
        res = client.post("/orders", json={**body, "symbol": "AAPL"})
        assert res.status_code == 200, body
        assert res.json()["accepted"] is True, body


def test_unknown_order_type_is_rejected_by_the_schema(client):
    res = client.post("/orders", json={"side": "buy", "price": 100.0,
                                       "quantity": 10, "order_type": "iceberg",
                                       "symbol": "AAPL"})
    assert res.status_code == 422


def test_stop_limit_without_a_limit_price_is_a_422(client):
    seed_market(client)
    res = client.post("/orders", json={"side": "sell", "price": 99.00,
                                       "quantity": 10,
                                       "order_type": "stop_limit",
                                       "stop_price": 99.00, "symbol": "AAPL"})
    assert res.status_code == 422
    assert "limit_price" in res.text


def test_stop_endpoint_reports_pending_then_triggered(client):
    seed_market(client)

    res = client.post("/orders", json={"side": "sell", "price": 99.00,
                                       "quantity": 50, "order_type": "stop",
                                       "stop_price": 99.00, "symbol": "AAPL"})
    body = res.json()
    assert body["status"] == "pending_trigger"
    assert client.get("/diagnostics").json()["pending_stops"] == 1

    # Sell through 99.00 to fire it.
    client.post("/orders", json={"side": "buy", "price": 98.50, "quantity": 200,
                                 "order_type": "limit", "symbol": "AAPL"})
    swept = client.post("/orders", json={"side": "sell", "price": 98.00,
                                         "quantity": 150, "order_type": "limit",
                                         "symbol": "AAPL"}).json()

    assert f"Triggered pending stop {body['order_id']}" in swept["message"]

    diagnostics = client.get("/diagnostics").json()
    assert diagnostics["pending_stops"] == 0
    assert diagnostics["stops_triggered"] == 1
    assert diagnostics["projection_repairs"] == 0


def test_pending_stop_is_cancelled_through_the_orders_endpoint(client):
    seed_market(client)
    order_id = client.post("/orders", json={"side": "sell", "price": 99.00,
                                            "quantity": 50,
                                            "order_type": "stop",
                                            "stop_price": 99.00,
                                            "symbol": "AAPL"}).json()["order_id"]

    assert client.delete(f"/orders/{order_id}").json()["accepted"] is True
    assert client.get("/diagnostics").json()["pending_stops"] == 0
    # Already gone — the same endpoint now reports not found.
    assert client.delete(f"/orders/{order_id}").json()["accepted"] is False


def test_websocket_snapshot_carries_pending_stops(client):
    seed_market(client)

    with client.websocket_connect("/ws/book") as ws:
        initial = ws.receive_json()
        assert initial["book"]["pending_stops"] == []

        client.post("/orders", json={"side": "sell", "price": 99.00,
                                     "quantity": 50, "order_type": "stop",
                                     "stop_price": 99.00, "symbol": "AAPL"})

        update = ws.receive_json()
        stops = update["book"]["pending_stops"]
        assert len(stops) == 1
        assert stops[0]["order_type"] == "stop"
        assert stops[0]["stop_price"] == 99.00
        assert stops[0]["quantity"] == 50
        # And it stays out of the book itself.
        assert update["book"]["asks"][0]["quantity"] == 100
        assert update["book"]["best_ask"] == 100.50
