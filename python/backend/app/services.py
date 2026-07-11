"""
EngineBridge — in-memory matching engine.

Implements price-time priority FIFO matching so the dashboard is fully
functional before pybind11 is wired in.  Swap out _match() and the data
structures for C++ calls when the bindings are ready; the WebSocket and
REST surface above stay identical.
"""

import asyncio
import json
import time
import uuid
from collections import defaultdict
from typing import Any, Dict, List, Optional, Set

from fastapi import WebSocket, WebSocketDisconnect

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


# ── Internal order representation ─────────────────────────────────────────────

class _Order:
    __slots__ = ("id", "side", "price", "quantity", "orig_quantity",
                 "order_type", "symbol", "seq", "created_at")

    def __init__(self, id_: int, side: str, price: float, quantity: int,
                 order_type: str, symbol: str, seq: int, created_at: str):
        self.id            = id_
        self.side          = side          # "buy" | "sell"
        self.price         = price
        self.quantity      = quantity
        self.orig_quantity = quantity
        self.order_type    = order_type
        self.symbol        = symbol
        self.seq           = seq           # insertion sequence for FIFO
        self.created_at    = created_at


# ── Engine ────────────────────────────────────────────────────────────────────

class EngineBridge:

    def __init__(self) -> None:
        self._next_order_id   = 1
        self._next_seq        = 0          # FIFO insertion counter
        self._next_fill_seq   = 0
        self._book_sequence   = 0

        # bids: descending by price, then ascending by seq (FIFO)
        # asks: ascending by price, then ascending by seq (FIFO)
        # Each side is a dict[price -> list[_Order]] (FIFO queue per level)
        self._bids: Dict[float, List[_Order]] = {}
        self._asks: Dict[float, List[_Order]] = {}

        # order_id -> _Order (only resting orders live here)
        self._order_map: Dict[int, _Order] = {}

        self._fills:      List[FillRecord] = []
        self._runs:       Dict[str, BacktestRun] = {}
        self._last_trade: Optional[float] = None

        # WebSocket connections
        self._connections: Set[WebSocket] = set()

    # ── REST handlers ─────────────────────────────────────────────────────────

    def submit_order(self, req: OrderRequest) -> OrderSubmissionResponse:
        order_id = self._next_order_id
        self._next_order_id += 1
        seq = self._next_seq
        self._next_seq += 1

        created_at = time.strftime("%H:%M:%S")
        order = _Order(order_id, req.side, req.price, req.quantity,
                       req.order_type, req.symbol, seq, created_at)

        if req.order_type == "market":
            self._match_market(order)
        elif req.order_type == "ioc":
            self._match_ioc(order)
        elif req.order_type == "fok":
            self._match_fok(order)
        else:
            # limit — match then rest
            self._match_limit(order)
            if order.quantity > 0:
                self._rest(order)

        self._book_sequence += 1
        self._schedule_broadcast()

        return OrderSubmissionResponse(
            accepted=True,
            order_id=order_id,
            status="accepted",
            message=f"Order {order_id} accepted.",
        )

    def cancel_order(self, req: CancelRequest) -> CancelResponse:
        order = self._order_map.pop(req.order_id, None)
        if order is None:
            return CancelResponse(
                accepted=False,
                order_id=req.order_id,
                message=f"Order {req.order_id} not found or already filled.",
            )

        side_book = self._bids if order.side == "buy" else self._asks
        level = side_book.get(order.price, [])
        side_book[order.price] = [o for o in level if o.id != order.id]
        if not side_book[order.price]:
            del side_book[order.price]

        self._book_sequence += 1
        self._schedule_broadcast()

        return CancelResponse(
            accepted=True,
            order_id=req.order_id,
            message=f"Order {req.order_id} cancelled.",
        )

    def start_backtest(self, req: BacktestRequest) -> BacktestRun:
        run_id = str(uuid.uuid4())[:8]
        run = BacktestRun(
            run_id=run_id, status="queued", symbol=req.symbol,
            start_time=req.start_time, end_time=req.end_time,
            depth=req.depth, message="Backtest queued for execution.",
        )
        self._runs[run_id] = run
        return run

    def get_backtest(self, run_id: str) -> BacktestRun:
        if run_id not in self._runs:
            raise KeyError(run_id)
        return self._runs[run_id]

    def get_fills(self, limit: int = 50) -> List[FillRecord]:
        return self._fills[-limit:]

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
        # send current state immediately
        await websocket.send_text(self._snapshot_json())
        try:
            while True:
                # keep-alive heartbeat; real updates are pushed by _broadcast()
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
        return json.dumps({
            "type": "snapshot",
            "book": snap.model_dump(),
            "fills": [f.model_dump() for f in self._fills[-50:]],
        })

    # ── Snapshot builder ──────────────────────────────────────────────────────

    def _build_snapshot(self) -> BookSnapshot:
        bids = [
            BookLevel(
                price=p,
                quantity=sum(o.quantity for o in orders),
                order_count=len(orders),
                order_ids=[o.id for o in orders],
                created_at_by_id={o.id: o.created_at for o in orders},
            )
            for p, orders in sorted(self._bids.items(), reverse=True)
            if orders
        ]
        asks = [
            BookLevel(
                price=p,
                quantity=sum(o.quantity for o in orders),
                order_count=len(orders),
                order_ids=[o.id for o in orders],
                created_at_by_id={o.id: o.created_at for o in orders},
            )
            for p, orders in sorted(self._asks.items())
            if orders
        ]

        resting = [
            RestingOrder(
                order_id=o.id, side=o.side, price=o.price,
                quantity=o.quantity, orig_quantity=o.orig_quantity,
                order_type=o.order_type, symbol=o.symbol,
                created_at=o.created_at,
            )
            for o in self._order_map.values()
        ]

        return BookSnapshot(
            symbol="AAPL",
            bids=bids,
            asks=asks,
            best_bid=bids[0].price if bids else None,
            best_ask=asks[0].price if asks else None,
            last_trade_price=self._last_trade,
            sequence=self._book_sequence,
            resting_orders=resting,
        )

    # ── Matching logic ────────────────────────────────────────────────────────

    def _rest(self, order: _Order) -> None:
        book = self._bids if order.side == "buy" else self._asks
        book.setdefault(order.price, []).append(order)
        self._order_map[order.id] = order

    def _match_limit(self, aggressive: _Order) -> None:
        """Match against opposite side until quantity exhausted or no cross."""
        opposite = self._asks if aggressive.side == "buy" else self._bids
        prices = (
            sorted(opposite.keys())          # ascending for buy (lowest ask first)
            if aggressive.side == "buy"
            else sorted(opposite.keys(), reverse=True)  # descending for sell
        )
        for price in prices:
            if aggressive.quantity == 0:
                break
            if aggressive.side == "buy" and price > aggressive.price:
                break
            if aggressive.side == "sell" and price < aggressive.price:
                break
            self._fill_at_level(aggressive, opposite, price)
        self._prune_empty(opposite)

    def _match_market(self, aggressive: _Order) -> None:
        """Match at any price until filled or book empty."""
        opposite = self._asks if aggressive.side == "buy" else self._bids
        prices = (
            sorted(opposite.keys())
            if aggressive.side == "buy"
            else sorted(opposite.keys(), reverse=True)
        )
        for price in prices:
            if aggressive.quantity == 0:
                break
            self._fill_at_level(aggressive, opposite, price)
        self._prune_empty(opposite)

    def _match_ioc(self, aggressive: _Order) -> None:
        """Fill immediately, cancel remainder."""
        self._match_limit(aggressive)
        # remainder is simply discarded (never rested)

    def _match_fok(self, aggressive: _Order) -> None:
        """Fill only if the full quantity can be satisfied; otherwise cancel."""
        opposite = self._asks if aggressive.side == "buy" else self._bids
        prices = (
            sorted(opposite.keys())
            if aggressive.side == "buy"
            else sorted(opposite.keys(), reverse=True)
        )
        available = 0
        for price in prices:
            if aggressive.side == "buy" and price > aggressive.price:
                break
            if aggressive.side == "sell" and price < aggressive.price:
                break
            available += sum(o.quantity for o in opposite.get(price, []))
            if available >= aggressive.quantity:
                break

        if available >= aggressive.quantity:
            self._match_limit(aggressive)

    def _fill_at_level(
        self,
        aggressive: _Order,
        opposite: Dict[float, List["_Order"]],
        price: float,
    ) -> None:
        queue = opposite.get(price, [])
        while queue and aggressive.quantity > 0:
            passive = queue[0]
            fill_qty = min(aggressive.quantity, passive.quantity)

            fill = FillRecord(
                sequence=self._next_fill_seq,
                aggressive_order_id=aggressive.id,
                passive_order_id=passive.id,
                side=aggressive.side,
                price=price,
                quantity=fill_qty,
                symbol=aggressive.symbol,
            )
            self._fills.append(fill)
            self._next_fill_seq += 1
            self._last_trade = price

            aggressive.quantity -= fill_qty
            passive.quantity    -= fill_qty

            if passive.quantity == 0:
                queue.pop(0)
                self._order_map.pop(passive.id, None)

    def _prune_empty(self, side_book: Dict[float, List["_Order"]]) -> None:
        empty = [p for p, q in side_book.items() if not q]
        for p in empty:
            del side_book[p]