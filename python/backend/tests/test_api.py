"""HTTP/WebSocket surface tests.

Covers the paths that only appear once FastAPI is driving the bridge — in
particular the WebSocket broadcast, which is scheduled from a worker thread
while the event loop runs on another.
"""

import pytest
from fastapi.testclient import TestClient

import app.main as main
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
