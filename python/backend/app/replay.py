"""Historical replay: drive the dashboard from a recorded ITCH 5.0 file.

A replay owns its own `TreeOrderBook` and `SimulationEngine`, kept entirely
separate from the live interactive book so starting one cannot disturb
orders a user has resting. `SimulationEngine.run()` blocks for the whole
session, so it runs on a daemon thread; its Python binding releases the GIL,
which is what lets the asyncio loop keep serving WebSockets while a replay
is in flight.

Threading
---------
`OrderBookInterface` is not thread-safe, and the engine mutates its book
continuously on the replay thread. Nothing outside that thread may touch it.
So the snapshot handed to the UI is built *inside* the engine's own snapshot
callback — on the engine thread, while the book is quiescent between events
— and stored as a plain dict. The serving thread only ever reads that dict
under a lock, never the book itself.
"""

from __future__ import annotations

import struct
import threading
import time
from pathlib import Path
from typing import Callable, Dict, List, Optional

import order_book as ob

# Replays emit a snapshot per event, which at demo pacing is far more than a
# browser needs. Collapse them to this cadence; the last state always wins.
_BROADCAST_INTERVAL_S = 0.05        # 20 updates/second

_MAX_FILLS = 50

_SIDE_TO_WIRE = {ob.Side.BID: "buy", ob.Side.ASK: "sell"}

_SPEEDS = {
    "fast": ob.ReplaySpeed.FAST,
    "realtime": ob.ReplaySpeed.REALTIME,
    "fixed": ob.ReplaySpeed.FIXED_NS,
}


def count_messages(path: Path) -> int:
    """Count framed messages by walking the length prefixes.

    Only the 2-byte headers are read, so this is cheap even for a large
    file, and it gives the UI a denominator for a progress bar — the
    replayer itself has no idea how many events are ahead of it.
    """
    total = 0
    with open(path, "rb") as handle:
        while True:
            header = handle.read(2)
            if len(header) < 2:
                return total
            length = struct.unpack(">H", header)[0]
            if length == 0:
                return total
            if len(handle.read(length)) < length:
                return total
            total += 1


class ReplayRunner:
    """One replay session: engine, book, thread and the state the UI reads."""

    def __init__(self, path: Path, symbol: str, speed: str,
                 fixed_ns: int, on_update: Callable[[], None]) -> None:
        self.path = path
        self.symbol = symbol
        self.speed = speed
        self.fixed_ns = fixed_ns

        self._on_update = on_update
        self._lock = threading.Lock()

        self._state = "starting"
        self._error: Optional[str] = None
        self._snapshot: Optional[Dict] = None
        self._fills: List[Dict] = []
        self._last_emit = 0.0
        self._started_at = time.time()

        self._total_messages = count_messages(path)

        config = ob.HistoricalReplayerConfig()
        config.path = str(path)
        config.symbol_filter = symbol
        config.speed = _SPEEDS[speed]
        config.fixed_ns = fixed_ns

        sim_config = ob.SimulationConfig()
        sim_config.symbol = symbol
        # NB: SimulationConfig.snapshot_depth is dead config — nothing in the
        # engine reads it, and TreeOrderBook::emit_snapshot hardcodes
        # snapshot(10). Callback snapshots are therefore always the top ten
        # levels per side, which is what the dashboard wants anyway. Setting
        # it here would imply a control that does not exist.

        self._engine = ob.SimulationEngine(
            sim_config, ob.TreeOrderBook(symbol, ob.FIFOMatcher()))
        # The engine installs its own composite callbacks on the book at
        # construction, so these must be registered on the engine rather
        # than on the TreeOrderBook — book-level ones would be overwritten.
        self._engine.set_snapshot_callback(self._on_snapshot)
        self._engine.set_fill_callback(self._on_fill)
        self._engine.add_source(ob.HistoricalReplayer(config))

        self._thread = threading.Thread(
            target=self._run, name="replay", daemon=True)

    # ── Lifecycle ─────────────────────────────────────────────────────────

    def start(self) -> None:
        self._state = "running"
        self._thread.start()

    def _run(self) -> None:
        try:
            self._engine.run()
            with self._lock:
                # stop() flips the state first; a natural end must not
                # overwrite it and claim the replay finished on its own.
                if self._state == "running":
                    self._state = "finished"
        except Exception as exc:                        # pragma: no cover
            with self._lock:
                self._state = "error"
                self._error = str(exc)
        finally:
            self._emit(force=True)

    def stop(self, timeout: float = 5.0) -> None:
        """Ask the engine to halt and wait for the thread to unwind.

        A paced replayer is asleep between events, and that sleep is not
        interruptible, so the engine notices `stop()` only once the current
        gap elapses — a few milliseconds at demo pacing.
        """
        with self._lock:
            if self._state == "running":
                self._state = "stopped"
        self._engine.stop()
        if self._thread.is_alive():
            self._thread.join(timeout=timeout)

    @property
    def finished(self) -> bool:
        return self._state in ("finished", "stopped", "error")

    # ── Engine-thread callbacks ───────────────────────────────────────────

    def _on_fill(self, event) -> None:
        with self._lock:
            self._fills.append({
                "sequence": event.sequence,
                "aggressive_order_id": event.aggressive_order_id,
                "passive_order_id": event.passive_order_id,
                "side": _SIDE_TO_WIRE[event.aggressor_side],
                "price": ob.from_basis_points(event.fill_price),
                "quantity": event.fill_quantity,
                "symbol": event.symbol or self.symbol,
            })
            if len(self._fills) > _MAX_FILLS:
                del self._fills[:-_MAX_FILLS]

    def _on_snapshot(self, snap) -> None:
        """Serialise the book on the engine thread, then throttle.

        Runs between events while the book is not being mutated, which is
        the only point it is safe to read.
        """
        bids = [self._level(lvl) for lvl in snap.bids]
        asks = [self._level(lvl) for lvl in snap.asks]

        payload = {
            "symbol": snap.symbol,
            "bids": bids,
            "asks": asks,
            # BookSnapshot carries no best_bid/best_ask of its own, and the
            # book itself must not be read from this thread, so take them
            # from the levels: bids descend, asks ascend, so each touch is
            # the head of its list.
            "best_bid": bids[0]["price"] if bids else None,
            "best_ask": asks[0]["price"] if asks else None,
            "last_trade_price": self._price(snap.last_trade_price),
            "sequence": snap.sequence,
            # A replay's orders are the feed's, not this user's. The UI
            # panels that list "your" resting orders and pending stops stay
            # empty rather than filling with thousands of anonymous rows.
            "resting_orders": [],
            "pending_stops": [],
        }
        with self._lock:
            self._snapshot = payload
        self._emit()

    @staticmethod
    def _price(value) -> Optional[float]:
        if value is None or value == 0:
            return None
        return ob.from_basis_points(value)

    @staticmethod
    def _level(level) -> Dict:
        return {
            "price": ob.from_basis_points(level.price),
            "quantity": level.quantity,
            "order_count": level.order_count,
            "order_ids": [],
            "created_at_by_id": {},
        }

    def _emit(self, force: bool = False) -> None:
        now = time.monotonic()
        if not force and now - self._last_emit < _BROADCAST_INTERVAL_S:
            return
        self._last_emit = now
        self._on_update()

    # ── State for the serving thread ──────────────────────────────────────

    def snapshot(self) -> Optional[Dict]:
        with self._lock:
            return self._snapshot

    def fills(self) -> List[Dict]:
        with self._lock:
            return list(self._fills)

    def status(self) -> Dict:
        replayer = self._engine.historical_replayer()
        replayed = replayer.orders_replayed() if replayer else 0
        with self._lock:
            state, error = self._state, self._error
        return {
            "state": state,
            "path": str(self.path),
            "symbol": self.symbol,
            "speed": self.speed,
            "fixed_ns": self.fixed_ns,
            "events_replayed": replayed,
            "events_total": self._total_messages,
            "fills": len(self._fills),
            "elapsed_seconds": round(time.time() - self._started_at, 1),
            "error": error,
        }
