"""
EngineBridge — adapter between the HTTP/WebSocket API and the C++ engine.

Every order submitted through this API is matched by the real `TreeOrderBook`
from the compiled `order_book` extension (see bindings/python/). This module
owns no matching logic of its own; it is a translation layer with three jobs:

  1. Vocabulary.   The wire format speaks "buy"/"sell", "ioc", and float
                   dollars. The engine speaks Side.BID, OrderType, and int64
                   basis points. Conversion happens here and nowhere else.

  2. Serialisation. OrderBookInterface is explicitly NOT thread-safe (see
                   order_book_interface.hpp) and expects a single caller.
                   FastAPI runs sync endpoints in a threadpool, so every
                   engine call is serialised under `_lock`.

  3. Projection.   The engine stores only what matching needs: price, side,
                   quantity, time priority. The dashboard also wants
                   submission timestamps, the original order type, and which
                   order IDs sit at each level — none of which the engine
                   models, and `has_order()` is only a bool. `_meta` is a
                   projection of exactly that extra metadata, rebuilt from the
                   engine's own fill/cancel callback stream so it cannot
                   drift silently.

Where a number exists in both places, the engine's value wins: level prices,
quantities and order counts are read from `book.snapshot(-1)`, never summed
from the projection.
"""

import asyncio
import json
import threading
import time
import uuid
from typing import Dict, List, Optional, Set

import order_book as ob
from fastapi import HTTPException, WebSocket, WebSocketDisconnect

from .models import (
    BacktestRequest,
    BacktestRun,
    BookLevel,
    BookSnapshot,
    CancelRequest,
    CancelResponse,
    FillRecord,
    OrderRequest,
    OrderSubmissionResponse,
    RestingOrder,
)

# ── Wire vocabulary → engine vocabulary ───────────────────────────────────────

_SIDE_TO_ENGINE = {"buy": ob.Side.BID, "sell": ob.Side.ASK}
_SIDE_TO_WIRE = {ob.Side.BID: "buy", ob.Side.ASK: "sell"}

# The UI offers these four. The engine also implements MARKET_LIMIT, STOP and
# STOP_LIMIT; stops are deliberately absent here because they must be routed
# through StopOrderManager, which has no Python binding yet — TreeOrderBook.add()
# throws on them by design. See README "Known gaps".
_TYPE_TO_ENGINE = {
    "limit": ob.OrderType.LIMIT,
    "market": ob.OrderType.MARKET,
    "ioc": ob.OrderType.IMMEDIATE_OR_CANCEL,
    "fok": ob.OrderType.FILL_OR_KILL,
}

# Engine cancel reasons, phrased for a human reading a flash message.
_CANCEL_BLURB = {
    ob.CancelReason.FOK_FAILED: "cancelled — fill-or-kill could not be filled in full",
    ob.CancelReason.IOC_EXPIRED: "immediate-or-cancel: remainder cancelled",
    ob.CancelReason.MARKET_NO_LIQUIDITY: "cancelled — insufficient liquidity",
    ob.CancelReason.CLIENT_REQUEST: "cancelled at client request",
    ob.CancelReason.RISK_REJECTED: "rejected by risk checks",
    ob.CancelReason.STOP_CANCELLED: "stop order cancelled",
    ob.CancelReason.SELF_TRADE: "cancelled — would have self-traded",
}

# Cancel reasons that mean "the order is gone for good", as opposed to a
# partial reduction that leaves the order resting.
_TERMINAL_REASONS = frozenset(_CANCEL_BLURB)


# ── Per-order metadata the engine doesn't model ───────────────────────────────

class _OrderMeta:
    """UI-facing metadata for one live order, plus its remaining quantity.

    `remaining` is maintained from FillEvent/CancelEvent callbacks rather than
    queried, because the engine exposes per-order existence (`has_order`) but
    not per-order size.
    """

    __slots__ = ("order_id", "side", "price", "order_type", "symbol",
                 "created_at", "orig_quantity", "remaining")

    def __init__(self, order_id: int, side: str, price: float, order_type: str,
                 symbol: str, created_at: str, quantity: int):
        self.order_id = order_id
        self.side = side
        self.price = price
        self.order_type = order_type
        self.symbol = symbol
        self.created_at = created_at
        self.orig_quantity = quantity
        self.remaining = quantity


class _Outcome:
    """Events the engine emitted while processing one submit_order call.

    Callbacks fire synchronously inside `book.add()`, so this is filled in
    before `add()` returns and read immediately afterwards — always under
    `_lock`, so one in-flight submission at a time.
    """

    __slots__ = ("order_id", "filled", "cancel_reason", "ack_status")

    def __init__(self, order_id: int):
        self.order_id = order_id
        self.filled = 0
        self.cancel_reason: Optional[ob.CancelReason] = None
        self.ack_status: Optional[ob.OrderStatus] = None


# ── Bridge ────────────────────────────────────────────────────────────────────

class EngineBridge:

    def __init__(self, symbol: str = "AAPL") -> None:
        self._symbol = symbol

        # OrderBookInterface is not thread-safe and FastAPI's sync endpoints
        # run in a threadpool — every engine touch goes through this lock.
        self._lock = threading.RLock()

        callbacks = ob.OrderBookCallbacks()
        callbacks.on_fill = self._on_fill
        callbacks.on_cancel = self._on_cancel
        callbacks.on_ack = self._on_ack
        callbacks.on_book_update = self._on_book_update

        self._book = ob.TreeOrderBook(symbol, ob.FIFOMatcher(), callbacks)

        self._next_order_id = 1
        self._meta: Dict[int, _OrderMeta] = {}
        self._fills: List[FillRecord] = []
        self._current: Optional[_Outcome] = None

        # Observability: the engine's own change signal, and how many phantom
        # projection entries we've had to drop (should stay at 0).
        self._engine_updates = 0
        self._projection_repairs = 0

        self._runs: Dict[str, BacktestRun] = {}
        self._connections: Set[WebSocket] = set()

    # ── Engine callbacks ──────────────────────────────────────────────────────
    #
    # These run on whichever thread called into the engine, inside add() /
    # cancel(), with `_lock` already held by the caller.

    def _on_ack(self, event) -> None:
        if self._current is not None and event.order_id == self._current.order_id:
            self._current.ack_status = event.status

    def _on_fill(self, event) -> None:
        self._fills.append(FillRecord(
            sequence=event.sequence,
            aggressive_order_id=event.aggressive_order_id,
            passive_order_id=event.passive_order_id,
            side=_SIDE_TO_WIRE[event.aggressor_side],
            price=ob.from_basis_points(event.fill_price),
            quantity=event.fill_quantity,
            symbol=event.symbol or self._symbol,
        ))

        # A fill consumes quantity on both sides of the trade. The aggressor
        # may not be in `_meta` as a resting order (a market order never
        # rests), and the passive side always is.
        self._consume(event.passive_order_id, event.fill_quantity)
        self._consume(event.aggressive_order_id, event.fill_quantity)

        if self._current is not None and \
                event.aggressive_order_id == self._current.order_id:
            self._current.filled += event.fill_quantity

    def _on_cancel(self, event) -> None:
        if event.reason in _TERMINAL_REASONS:
            self._meta.pop(event.order_id, None)

        if self._current is not None and event.order_id == self._current.order_id:
            self._current.cancel_reason = event.reason

    def _on_book_update(self, snapshot) -> None:
        # The engine's authoritative "book changed" signal. The WebSocket
        # broadcast isn't driven from here because this runs on a worker
        # thread with no event loop — the request handler schedules it once
        # the engine call has returned.
        self._engine_updates += 1

    def _consume(self, order_id: int, quantity: int) -> None:
        meta = self._meta.get(order_id)
        if meta is None:
            return
        meta.remaining -= quantity
        if meta.remaining <= 0:
            self._meta.pop(order_id, None)

    # ── REST handlers ─────────────────────────────────────────────────────────

    def submit_order(self, req: OrderRequest) -> OrderSubmissionResponse:
        engine_type = _TYPE_TO_ENGINE[req.order_type]

        # MARKET orders ignore the price field entirely (order_validators.cpp
        # skips the positive-price check for them), so don't pretend otherwise
        # — the UI disables the input but still posts whatever was last typed.
        price_bp = 0 if engine_type == ob.OrderType.MARKET \
            else ob.to_basis_points(req.price)

        with self._lock:
            order_id = self._next_order_id
            self._next_order_id += 1

            order = ob.Order(
                id=order_id,
                symbol=req.symbol,
                side=_SIDE_TO_ENGINE[req.side],
                type=engine_type,
                price=price_bp,
                quantity=req.quantity,
                timestamp=time.time_ns(),
            )

            # Registered before add() because fill/cancel callbacks fire
            # *during* add() and look this entry up. If the order fully fills
            # or is cancelled, those same callbacks remove it again.
            self._meta[order_id] = _OrderMeta(
                order_id=order_id,
                side=req.side,
                price=ob.from_basis_points(price_bp),
                order_type=req.order_type,
                symbol=req.symbol,
                created_at=time.strftime("%H:%M:%S"),
                quantity=req.quantity,
            )

            self._current = _Outcome(order_id)
            try:
                self._book.add(order)
                outcome = self._current
            finally:
                self._current = None

        self._schedule_broadcast()
        return self._describe(outcome, req)

    def _describe(self, outcome: _Outcome,
                  req: OrderRequest) -> OrderSubmissionResponse:
        """Turn the engine's event stream for one order into an API response."""
        filled = outcome.filled
        order_id = outcome.order_id

        if outcome.cancel_reason is not None:
            blurb = _CANCEL_BLURB.get(outcome.cancel_reason, "cancelled")
            # An IOC that filled some of its quantity is a success with a
            # cancelled tail; FOK/no-liquidity cancels filled nothing.
            if filled:
                return OrderSubmissionResponse(
                    accepted=True,
                    order_id=order_id,
                    status="partially_filled",
                    message=f"Order {order_id} filled {filled}/{req.quantity} — {blurb}.",
                )
            return OrderSubmissionResponse(
                accepted=False,
                order_id=order_id,
                status="cancelled",
                message=f"Order {order_id} {blurb}.",
            )

        if filled >= req.quantity:
            return OrderSubmissionResponse(
                accepted=True,
                order_id=order_id,
                status="filled",
                message=f"Order {order_id} fully filled ({filled}).",
            )

        if filled:
            return OrderSubmissionResponse(
                accepted=True,
                order_id=order_id,
                status="accepted",
                message=(f"Order {order_id} accepted — filled {filled}/"
                         f"{req.quantity}, remainder resting."),
            )

        return OrderSubmissionResponse(
            accepted=True,
            order_id=order_id,
            status="accepted",
            message=f"Order {order_id} accepted.",
        )

    def cancel_order(self, req: CancelRequest) -> CancelResponse:
        with self._lock:
            # TreeOrderBook::cancel() is a silent no-op for unknown IDs, so
            # ask first to distinguish "cancelled" from "never existed".
            if not self._book.has_order(req.order_id):
                return CancelResponse(
                    accepted=False,
                    order_id=req.order_id,
                    message=f"Order {req.order_id} not found or already filled.",
                )
            self._book.cancel(req.order_id)

        self._schedule_broadcast()
        return CancelResponse(
            accepted=True,
            order_id=req.order_id,
            message=f"Order {req.order_id} cancelled.",
        )

    def get_fills(self, limit: int = 50) -> List[FillRecord]:
        with self._lock:
            return self._fills[-limit:]

    # ── Backtests ─────────────────────────────────────────────────────────────
    #
    # Not implemented. Running one means driving HistoricalReplayer over an
    # ITCH 5.0 file, which this service has no way to obtain (see README
    # "Known gaps"). The endpoints stay so the route shape is settled, but
    # they report their real status rather than queueing work nothing runs.

    def start_backtest(self, req: BacktestRequest) -> BacktestRun:
        run_id = str(uuid.uuid4())[:8]
        run = BacktestRun(
            run_id=run_id, status="not_implemented", symbol=req.symbol,
            start_time=req.start_time, end_time=req.end_time, depth=req.depth,
            message=("Backtest execution is not implemented: no ITCH 5.0 data "
                     "source is configured for this deployment."),
        )
        self._runs[run_id] = run
        return run

    def get_backtest(self, run_id: str) -> BacktestRun:
        run = self._runs.get(run_id)
        if run is None:
            raise HTTPException(status_code=404,
                                detail=f"No backtest run with id {run_id}.")
        return run

    # ── Snapshot ──────────────────────────────────────────────────────────────

    def _build_snapshot(self) -> BookSnapshot:
        """Compose the engine's book state with the UI-only projection.

        Level prices, quantities and order counts come from the engine.
        Order IDs, submission times and the resting-order list come from
        `_meta`, filtered through `has_order()` so a stale projection entry
        can never surface as a phantom order.
        """
        with self._lock:
            snap = self._book.snapshot(-1)   # -1 = all levels, not just top 10

            live: List[_OrderMeta] = []
            for meta in list(self._meta.values()):
                if self._book.has_order(meta.order_id):
                    live.append(meta)
                else:
                    # The engine says it's gone but we still had it — drop it
                    # and count the repair. Should never happen; if it does,
                    # a callback was missed.
                    self._meta.pop(meta.order_id, None)
                    self._projection_repairs += 1

            # Group live order IDs by level, preserving submission order,
            # which for a FIFO book is time priority.
            by_level: Dict[tuple, List[_OrderMeta]] = {}
            for meta in live:
                by_level.setdefault((meta.side, round(meta.price, 4)), []).append(meta)

            def levels(engine_levels, side: str) -> List[BookLevel]:
                out = []
                for lvl in engine_levels:
                    price = ob.from_basis_points(lvl.price)
                    members = by_level.get((side, round(price, 4)), [])
                    out.append(BookLevel(
                        price=price,
                        quantity=lvl.quantity,       # engine is authoritative
                        order_count=lvl.order_count,  # engine is authoritative
                        order_ids=[m.order_id for m in members],
                        created_at_by_id={m.order_id: m.created_at for m in members},
                    ))
                return out

            best_bid = self._book.best_bid()
            best_ask = self._book.best_ask()
            last_trade = self._book.last_trade_price()

            return BookSnapshot(
                symbol=snap.symbol,
                bids=levels(snap.bids, "buy"),
                asks=levels(snap.asks, "sell"),
                best_bid=ob.from_basis_points(best_bid) if best_bid is not None else None,
                best_ask=ob.from_basis_points(best_ask) if best_ask is not None else None,
                last_trade_price=ob.from_basis_points(last_trade) if last_trade else None,
                sequence=snap.sequence,
                resting_orders=[
                    RestingOrder(
                        order_id=m.order_id, side=m.side, price=m.price,
                        quantity=m.remaining, orig_quantity=m.orig_quantity,
                        order_type=m.order_type, symbol=m.symbol,
                        created_at=m.created_at,
                    )
                    for m in live
                ],
            )

    def diagnostics(self) -> Dict[str, int]:
        """Engine-vs-projection health, surfaced for debugging."""
        with self._lock:
            return {
                "engine_updates": self._engine_updates,
                "engine_sequence": self._book.sequence(),
                "projection_size": len(self._meta),
                "projection_repairs": self._projection_repairs,
                "fills": len(self._fills),
            }

    # ── WebSocket ─────────────────────────────────────────────────────────────

    def _schedule_broadcast(self) -> None:
        try:
            loop = asyncio.get_running_loop()
        except RuntimeError:
            loop = None

        if loop is not None and loop.is_running():
            loop.create_task(self._broadcast())
            return

        asyncio.run(self._broadcast())

    async def connect(self, websocket: WebSocket) -> None:
        await websocket.accept()
        self._connections.add(websocket)
        await websocket.send_text(self._snapshot_json())
        try:
            while True:
                # Keep-alive resend; real updates are pushed by _broadcast().
                await asyncio.sleep(5.0)
                await websocket.send_text(self._snapshot_json())
        except (WebSocketDisconnect, Exception):
            self._connections.discard(websocket)

    async def _broadcast(self) -> None:
        if not self._connections:
            return
        msg = self._snapshot_json()
        dead: Set[WebSocket] = set()
        for ws in self._connections:
            try:
                await ws.send_text(msg)
            except Exception:
                dead.add(ws)
        self._connections -= dead

    def _snapshot_json(self) -> str:
        snap = self._build_snapshot()
        with self._lock:
            recent = [f.model_dump() for f in self._fills[-50:]]
        return json.dumps({
            "type": "snapshot",
            "book": snap.model_dump(),
            "fills": recent,
        })
