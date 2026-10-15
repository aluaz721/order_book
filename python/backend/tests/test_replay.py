"""Historical replay: the runner, the routes, and the path guard.

These build their own small ITCH 5.0 file rather than depending on whatever
sits in the repo's data/ directory, so a replay test is deterministic and
does not need `scripts/generate_itch.py` to have been run first. The encoder
below is deliberately independent of the production parser, same as the C++
replayer's own tests — it exercises the wire format rather than checking the
parser agrees with itself.
"""

import struct
import time

import pytest
from fastapi.testclient import TestClient

import app.main as main
from app.services import EngineBridge


# ── A minimal ITCH 5.0 encoder ────────────────────────────────────────────────

def _u16(v): return struct.pack(">H", v)
def _u32(v): return struct.pack(">I", v)
def _u48(v): return v.to_bytes(6, "big")
def _u64(v): return struct.pack(">Q", v)
def _stock(s): return s.encode().ljust(8)


def _framed(payload: bytes) -> bytes:
    return _u16(len(payload)) + payload


def _add(ref, side, shares, price_bp, ts, symbol="AAPL", locate=1) -> bytes:
    return (b"A" + _u16(locate) + _u16(0) + _u48(ts) + _u64(ref) +
            side.encode() + _u32(shares) + _stock(symbol) + _u32(price_bp))


def _execute(ref, shares, ts, locate=1) -> bytes:
    return b"E" + _u16(locate) + _u16(0) + _u48(ts) + _u64(ref) + _u32(shares) + _u64(0)


# Two passive adds and an execution against the resting bid: one fill, and a
# book with liquidity left on both sides when the file runs out.
_SESSION = (
    _framed(_add(1, "B", 100, 1_500_000, 1_000)) +
    _framed(_add(2, "S", 40, 1_500_500, 2_000)) +
    _framed(_execute(1, 30, 3_000))
)


@pytest.fixture
def data_dir(tmp_path, monkeypatch):
    """A data directory holding one replayable file, plus a decoy to ignore."""
    (tmp_path / "sample.itch").write_bytes(_SESSION)
    (tmp_path / "notes.txt").write_text("not a replay file")
    monkeypatch.setenv("ORDER_BOOK_DATA_DIR", str(tmp_path))
    return tmp_path


@pytest.fixture
def client(data_dir):
    # EngineBridge reads ORDER_BOOK_DATA_DIR in __init__, so it has to be
    # constructed after the fixture above has set it.
    main.bridge = EngineBridge()
    with TestClient(main.app) as c:
        yield c


def _wait_until_finished(client, timeout=5.0):
    """Poll /replay/status until the runner reports it is done."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        status = client.get("/replay/status").json()
        if status["state"] in ("finished", "stopped", "error"):
            return status
        time.sleep(0.02)
    pytest.fail(f"replay did not finish within {timeout}s: "
                f"{client.get('/replay/status').json()}")


# ── Discovery ─────────────────────────────────────────────────────────────────

def test_only_itch_files_are_offered(client):
    assert client.get("/replay/files").json() == ["sample.itch"]


def test_status_is_null_before_any_replay(client):
    assert client.get("/replay/status").json() is None


# ── Running one ───────────────────────────────────────────────────────────────

def test_replay_runs_to_completion_and_drives_the_book(client):
    response = client.post("/replay/start", json={
        "path": "sample.itch", "symbol": "AAPL", "speed": "fast",
    })
    assert response.status_code == 200
    assert response.json()["state"] in ("starting", "running", "finished")

    status = _wait_until_finished(client)
    assert status["state"] == "finished"
    assert status["events_replayed"] == 3
    assert status["events_total"] == 3
    assert status["fills"] == 1
    assert status["error"] is None


def test_replay_snapshot_reaches_the_websocket(client):
    with client.websocket_connect("/ws/book") as ws:
        ws.receive_json()  # the frame sent on connect, before any replay

        client.post("/replay/start", json={
            "path": "sample.itch", "symbol": "AAPL", "speed": "fast",
        })
        _wait_until_finished(client)

        # The replay's own book is what the socket carries while one exists,
        # and it keeps the shape a live frame has. Frames arrive from the first
        # Add onward, so wait for one with both sides resting rather than
        # taking the first that has a bid.
        for _ in range(20):
            frame = ws.receive_json()
            assert frame["type"] == "snapshot"
            if frame.get("replay") and frame["book"]["bids"] and frame["book"]["asks"]:
                break
        else:
            pytest.fail("no replay frame carried a two-sided book")

        book = frame["book"]
        assert book["best_bid"] == pytest.approx(150.00)
        assert book["best_ask"] == pytest.approx(150.05)
        # A replay's orders are the feed's, not this user's.
        assert book["resting_orders"] == []
        assert book["pending_stops"] == []


def test_live_book_is_untouched_by_a_replay(client):
    """A user's resting order has to survive a replay — separate books."""
    submitted = client.post("/orders", json={
        "side": "buy", "price": 99.50, "quantity": 5,
        "order_type": "limit", "symbol": "AAPL",
    }).json()
    assert submitted["accepted"]

    client.post("/replay/start", json={"path": "sample.itch", "speed": "fast"})
    _wait_until_finished(client)
    client.post("/replay/stop")

    # Back on the live book, the order is still resting where it was.
    assert main.bridge._book.has_order(submitted["order_id"])
    assert main.bridge._book.best_bid() == 995_000


# ── Stopping ──────────────────────────────────────────────────────────────────

def test_stop_reports_stopped_and_is_idempotent_enough_to_restart(client):
    client.post("/replay/start", json={
        "path": "sample.itch", "speed": "fixed", "fixed_ns": 50_000_000,
    })
    stopped = client.post("/replay/stop").json()
    assert stopped["state"] == "stopped"

    # A stopped replay is finished, so a new one may start.
    assert client.post("/replay/start", json={
        "path": "sample.itch", "speed": "fast",
    }).status_code == 200


def test_stop_without_a_replay_is_a_404(client):
    assert client.post("/replay/stop").status_code == 404


def test_second_replay_while_one_runs_is_a_409(client):
    # Paced slowly enough that it is still running when the second call lands.
    client.post("/replay/start", json={
        "path": "sample.itch", "speed": "fixed", "fixed_ns": 200_000_000,
    })
    conflict = client.post("/replay/start", json={"path": "sample.itch"})
    assert conflict.status_code == 409
    assert "already running" in conflict.json()["detail"]

    client.post("/replay/stop")


# ── Input the endpoint must refuse ────────────────────────────────────────────

@pytest.mark.parametrize("escape", [
    "../../etc/passwd",
    "../sample.itch",
    "/etc/passwd",
    "subdir/../../../etc/hosts",
])
def test_paths_outside_the_data_directory_are_refused(client, escape):
    """`path` arrives over HTTP, so it must not be able to leave the root."""
    response = client.post("/replay/start", json={"path": escape})
    assert response.status_code in (400, 404)
    assert "etc" not in response.json()["detail"]


def test_unknown_file_is_a_404(client):
    response = client.post("/replay/start", json={"path": "nope.itch"})
    assert response.status_code == 404
    assert "nope.itch" in response.json()["detail"]


def test_non_positive_fixed_gap_is_a_422(client):
    response = client.post("/replay/start", json={
        "path": "sample.itch", "speed": "fixed", "fixed_ns": 0,
    })
    assert response.status_code == 422


def test_unknown_speed_is_rejected_by_the_schema(client):
    response = client.post("/replay/start", json={
        "path": "sample.itch", "speed": "warp",
    })
    assert response.status_code == 422
