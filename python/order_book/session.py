"""
Session — a single-symbol trading session over the C++ engine.

The compiled bindings mirror the C++ API exactly: prices are int64 basis
points, a book is driven through a SimulationEngine, orders arrive through an
OrderSource, and STOP / STOP_LIMIT orders are held by a StopOrderManager
rather than the book. That is the right shape for the engine and an awkward
one to type at a REPL, so Session wires the pieces together once and exposes
the part a caller actually wants: submit an order of any of the seven types,
in dollars, and get back what happened.

    import order_book as ob

    s = ob.Session("AAPL")
    s.limit("sell", 100, 150.00)
    s.limit("buy",  100, 149.50)
    s.stop("sell",   50, stop_price=149.00)   # held outside the book
    result = s.market("sell", 150)            # sweeps, and fires the stop

    result.filled_quantity                     # 150
    s.pending_stops                            # 0 — the stop fired

Everything Session does is also doable directly against the bindings; see
`Session.engine` and `Session.book` to drop down to them at any point.

Units: Session's own arguments and returns are dollars (floats), because that
is what a caller types. The event objects it hands back are the engine's own
(FillEvent, CancelEvent, AckEvent, TriggerEvent, BookSnapshot) and therefore
still carry int64 basis points — use `order_book.from_basis_points()` on their
price fields.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, List, Optional, Sequence, Tuple

from . import _order_book as _ob

__all__ = ["Session", "OrderResult"]


# Accepted spellings for a side. The engine models a resting buy as a BID.
_SIDES = {
    "bid": _ob.Side.BID, "buy": _ob.Side.BID, "b": _ob.Side.BID,
    "ask": _ob.Side.ASK, "sell": _ob.Side.ASK, "s": _ob.Side.ASK,
}

_TYPES = {
    "limit": _ob.OrderType.LIMIT,
    "market": _ob.OrderType.MARKET,
    "market_limit": _ob.OrderType.MARKET_LIMIT,
    "stop": _ob.OrderType.STOP,
    "stop_limit": _ob.OrderType.STOP_LIMIT,
    "ioc": _ob.OrderType.IMMEDIATE_OR_CANCEL,
    "immediate_or_cancel": _ob.OrderType.IMMEDIATE_OR_CANCEL,
    "fok": _ob.OrderType.FILL_OR_KILL,
    "fill_or_kill": _ob.OrderType.FILL_OR_KILL,
}

_STOP_TYPES = (_ob.OrderType.STOP, _ob.OrderType.STOP_LIMIT)

# Converted stop orders and auto-assigned strategy orders draw ids from the
# engine's counter, which starts here. Session's own ids start at 1, so the
# two ranges never collide and a triggered order is recognisable by its id.
_ENGINE_ID_START = 1_000_000


def _coerce_side(side) -> "_ob.Side":
    if isinstance(side, _ob.Side):
        return side
    try:
        return _SIDES[str(side).strip().lower()]
    except KeyError:
        raise ValueError(
            f"unknown side {side!r} — use 'buy'/'sell' or order_book.Side"
        ) from None


def _coerce_type(order_type) -> "_ob.OrderType":
    if isinstance(order_type, _ob.OrderType):
        return order_type
    try:
        return _TYPES[str(order_type).strip().lower()]
    except KeyError:
        raise ValueError(
            f"unknown order type {order_type!r} — one of "
            f"{sorted(set(_TYPES))} or order_book.OrderType"
        ) from None


@dataclass(frozen=True)
class OrderResult:
    """What one submission did, as seen from outside the engine.

    The event tuples hold only the events this submission produced — a
    triggered stop's own fills included, since those are a consequence of it.
    """

    order_id: int
    status: Optional["_ob.OrderStatus"]
    message: str
    fills: Tuple["_ob.FillEvent", ...]
    cancels: Tuple["_ob.CancelEvent", ...]
    triggers: Tuple["_ob.TriggerEvent", ...]

    @property
    def filled_quantity(self) -> int:
        """Quantity this order traded. Excludes fills by triggered stops."""
        return sum(f.fill_quantity for f in self.fills
                   if f.aggressive_order_id == self.order_id)

    @property
    def traded_quantity(self) -> int:
        """Quantity traded in total, including by any stop this order fired."""
        return sum(f.fill_quantity for f in self.fills)

    @property
    def rejected(self) -> bool:
        return self.status == _ob.OrderStatus.REJECTED

    @property
    def pending_trigger(self) -> bool:
        """True for a stop that is now held, waiting for its trigger price."""
        return self.status == _ob.OrderStatus.PENDING_TRIGGER

    def __repr__(self) -> str:
        status = self.status.name if self.status is not None else "QUEUED"
        return (f"<OrderResult id={self.order_id} {status} "
                f"filled={self.filled_quantity} "
                f"fills={len(self.fills)} triggers={len(self.triggers)}"
                + (f" — {self.message}" if self.message else "") + ">")


class Session:
    """A book, an engine, and an order queue for one symbol.

    Submission drains the engine immediately by default (`run=False` to batch,
    then call `run()`), which is the same pattern the FastAPI layer uses: the
    engine's `run()` returns as soon as its live sources are empty.
    """

    def __init__(self,
                 symbol: str = "AAPL",
                 *,
                 matcher: Optional["_ob.MatchingAlgorithm"] = None,
                 snapshot_depth: int = 10,
                 strategy: Optional[Callable] = None) -> None:
        config = _ob.SimulationConfig()
        config.symbol = symbol
        config.snapshot_depth = snapshot_depth
        config.order_id_start = _ENGINE_ID_START

        book = _ob.TreeOrderBook(symbol, matcher or _ob.FIFOMatcher())
        self._engine = _ob.SimulationEngine(config, book)
        self._engine.add_source(_ob.InteractiveSource())

        self.symbol = symbol
        self.fills: List["_ob.FillEvent"] = []
        self.cancels: List["_ob.CancelEvent"] = []
        self.acks: List["_ob.AckEvent"] = []
        self.triggers: List["_ob.TriggerEvent"] = []
        self.snapshots: List["_ob.BookSnapshot"] = []

        self._engine.set_fill_callback(self.fills.append)
        self._engine.set_cancel_callback(self.cancels.append)
        self._engine.set_ack_callback(self.acks.append)
        self._engine.set_trigger_callback(self.triggers.append)
        self._engine.set_snapshot_callback(self.snapshots.append)

        self._next_id = 1
        if strategy is not None:
            self.set_strategy(strategy)

    # ── Access to the layers underneath ──────────────────────────────────────

    @property
    def engine(self) -> "_ob.SimulationEngine":
        return self._engine

    @property
    def book(self) -> "_ob.OrderBookInterface":
        return self._engine.book()

    @property
    def stop_manager(self) -> "_ob.StopOrderManager":
        return self._engine.stop_manager()

    @property
    def source(self) -> "_ob.InteractiveSource":
        return self._engine.interactive_source()

    # ── Submission ───────────────────────────────────────────────────────────

    def submit(self,
               side,
               order_type="limit",
               quantity: int = 0,
               price: Optional[float] = None,
               *,
               stop_price: Optional[float] = None,
               limit_price: Optional[float] = None,
               order_id: Optional[int] = None,
               timestamp: Optional[int] = None,
               run: bool = True) -> OrderResult:
        """Submit an order of any of the seven types. Prices in dollars.

        `price` is the resting/limit price for LIMIT, MARKET_LIMIT, IOC and
        FOK, and is ignored for MARKET. For STOP and STOP_LIMIT pass
        `stop_price` (the trigger), plus `limit_price` for STOP_LIMIT.

        Only missing arguments are checked here — whether an order makes sense
        against the current market (a stop on the wrong side of it, a FOK with
        insufficient liquidity) is the engine's judgement, and comes back as a
        rejected `OrderResult` rather than an exception.
        """
        side_enum = _coerce_side(side)
        type_enum = _coerce_type(order_type)

        if quantity <= 0:
            raise ValueError("quantity must be positive")

        if type_enum in _STOP_TYPES:
            trigger = stop_price if stop_price is not None else price
            if trigger is None:
                raise ValueError(
                    f"{type_enum.name} needs a stop_price (the trigger price)")
            price_bp = _ob.to_basis_points(trigger)
            if type_enum == _ob.OrderType.STOP_LIMIT:
                if limit_price is None:
                    raise ValueError("STOP_LIMIT needs a limit_price")
                limit_bp = _ob.to_basis_points(limit_price)
            else:
                limit_bp = 0
        else:
            if price is None:
                if type_enum != _ob.OrderType.MARKET:
                    raise ValueError(f"{type_enum.name} needs a price")
                price_bp = 0
            else:
                price_bp = _ob.to_basis_points(price)
            limit_bp = (_ob.to_basis_points(limit_price)
                        if limit_price is not None else 0)

        oid = order_id if order_id is not None else self._claim_order_id()

        order = _ob.Order(
            id=oid,
            symbol=self.symbol,
            side=side_enum,
            type=type_enum,
            price=price_bp,
            quantity=quantity,
            timestamp=timestamp if timestamp is not None else 0,
            limit_price=limit_bp,
        )

        mark = self._mark()
        self.source.submit(order)
        if run:
            self._engine.run()
        return self._result_since(mark, oid)

    # Per-type shorthands. Each one exists so the required arguments for that
    # type are visible in the signature instead of in a docstring.

    def limit(self, side, quantity: int, price: float, **kwargs) -> OrderResult:
        return self.submit(side, "limit", quantity, price, **kwargs)

    def market(self, side, quantity: int, **kwargs) -> OrderResult:
        return self.submit(side, "market", quantity, **kwargs)

    def market_limit(self, side, quantity: int, price: float,
                     **kwargs) -> OrderResult:
        """MARKET that rests its unfilled remainder at the last traded price.

        `price` is required by the engine's field validation but ignored when
        matching — the remainder rests at the traded price, not at this one.
        """
        return self.submit(side, "market_limit", quantity, price, **kwargs)

    def ioc(self, side, quantity: int, price: float, **kwargs) -> OrderResult:
        return self.submit(side, "ioc", quantity, price, **kwargs)

    def fok(self, side, quantity: int, price: float, **kwargs) -> OrderResult:
        return self.submit(side, "fok", quantity, price, **kwargs)

    def stop(self, side, quantity: int, stop_price: float,
             **kwargs) -> OrderResult:
        """A stop that becomes a MARKET order when the market crosses it.

        Held outside the book until then, so it does not show in depth. A buy
        stop must sit above the market and a sell stop below it, or the engine
        rejects it as one that would fire immediately.
        """
        return self.submit(side, "stop", quantity, stop_price=stop_price,
                           **kwargs)

    def stop_limit(self, side, quantity: int, stop_price: float,
                   limit_price: float, **kwargs) -> OrderResult:
        """A stop that becomes a LIMIT order at `limit_price` when it fires.

        Convention (see cpp/include/order_book/orders/order_validators.hpp):
        `limit_price` <= `stop_price` for buys, >= for sells.
        """
        return self.submit(side, "stop_limit", quantity, stop_price=stop_price,
                           limit_price=limit_price, **kwargs)

    # ── Cancellation ─────────────────────────────────────────────────────────

    def cancel(self, order_id: int, *, run: bool = True) -> bool:
        """Cancel a resting order or a stop that has not triggered yet.

        Returns False if the id is neither — already filled, already
        triggered, or never seen.
        """
        if self.stop_manager.has_stop(order_id):
            self.stop_manager.cancel(order_id, self._engine.current_time())
            return True

        known = self.book.has_order(order_id)
        self.source.submit_cancel(order_id, self.symbol)
        if run:
            self._engine.run()
        return known

    # ── Strategy callback ────────────────────────────────────────────────────

    def set_strategy(self, callback: Callable) -> "_ob.StrategySource":
        """Run `callback(snapshot, timestamp) -> list[Order]` on book updates.

        Orders it returns are dispatched on the following tick — the engine
        deliberately won't let a strategy fill against the very snapshot it
        reacted to. Build them with `make_order()`, which assigns ids; the
        engine rejects an order with id 0, and StrategySource (unlike the
        interactive queue) does not fill one in.

        A callback that returns an order on every invocation feeds itself
        forever, since each resulting book update invokes it again. Return an
        empty list when there is nothing to do.
        """
        existing = self._engine.strategy_source()
        wrapped = self._wrap_strategy(callback)
        if existing is not None:
            existing.set_callback(wrapped)
            return existing

        source = _ob.StrategySource(wrapped)
        self._engine.add_source(source)
        return self._engine.strategy_source()

    def make_order(self, side, order_type="limit", quantity: int = 0,
                   price: Optional[float] = None, *,
                   stop_price: Optional[float] = None,
                   limit_price: Optional[float] = None,
                   order_id: Optional[int] = None,
                   timestamp: int = 0) -> "_ob.Order":
        """Build an Order (dollar prices, auto id) without submitting it.

        For use inside a strategy callback, which returns orders rather than
        submitting them.
        """
        side_enum = _coerce_side(side)
        type_enum = _coerce_type(order_type)

        if type_enum in _STOP_TYPES:
            trigger = stop_price if stop_price is not None else price
            if trigger is None:
                raise ValueError(f"{type_enum.name} needs a stop_price")
            price_bp = _ob.to_basis_points(trigger)
        elif price is None:
            price_bp = 0
        else:
            price_bp = _ob.to_basis_points(price)

        return _ob.Order(
            id=order_id if order_id is not None else self._claim_order_id(),
            symbol=self.symbol,
            side=side_enum,
            type=type_enum,
            price=price_bp,
            quantity=quantity,
            timestamp=timestamp,
            limit_price=(_ob.to_basis_points(limit_price)
                         if limit_price is not None else 0),
        )

    # ── Engine lifecycle ─────────────────────────────────────────────────────

    def run(self) -> None:
        """Drain everything queued. Returns when the queues are empty."""
        self._engine.run()

    def reset(self) -> None:
        """Clear recorded events and engine statistics.

        The book itself is not cleared — the engine has no way to clear it.
        Build a new Session for a clean book.
        """
        self._engine.reset()
        for log in (self.fills, self.cancels, self.acks, self.triggers,
                    self.snapshots):
            log.clear()

    # ── Book state, in dollars ───────────────────────────────────────────────

    def snapshot(self, depth: int = 10) -> "_ob.BookSnapshot":
        return self.book.snapshot(depth)

    def best_bid(self) -> Optional[float]:
        return self._dollars(self.book.best_bid())

    def best_ask(self) -> Optional[float]:
        return self._dollars(self.book.best_ask())

    def mid_price(self) -> Optional[float]:
        return self._dollars(self.book.mid_price())

    def spread(self) -> Optional[float]:
        return self._dollars(self.book.spread())

    def last_trade_price(self) -> Optional[float]:
        price = self.book.last_trade_price()
        return _ob.from_basis_points(price) if price else None

    @property
    def pending_stops(self) -> int:
        """Stops accepted but not yet triggered."""
        return self.stop_manager.pending_stop_count()

    def nearest_stop(self, side) -> Optional[float]:
        """The pending stop closest to firing on one side, in dollars."""
        side_enum = _coerce_side(side)
        price = (self.stop_manager.nearest_stop_buy()
                 if side_enum == _ob.Side.BID
                 else self.stop_manager.nearest_stop_sell())
        return _ob.from_basis_points(price) if price else None

    def validate(self, order: "_ob.Order") -> "_ob.ValidationResult":
        """Ask the engine's validators about an order without submitting it."""
        return _ob.validators.validate(order, self.book)

    def __repr__(self) -> str:
        return (f"<Session {self.symbol} bid={self.best_bid()} "
                f"ask={self.best_ask()} "
                f"orders={self._engine.orders_processed()} "
                f"pending_stops={self.pending_stops}>")

    # ── Internals ────────────────────────────────────────────────────────────

    def _claim_order_id(self) -> int:
        oid = self._next_id
        self._next_id += 1
        return oid

    def _wrap_strategy(self, callback: Callable) -> Callable:
        def wrapped(snapshot, timestamp) -> List["_ob.Order"]:
            orders = callback(snapshot, timestamp) or []
            if isinstance(orders, _ob.Order):
                orders = [orders]
            for order in orders:
                if order.id == 0:
                    order.id = self._claim_order_id()
            return list(orders)
        return wrapped

    def _mark(self) -> Sequence[int]:
        return (len(self.fills), len(self.cancels),
                len(self.acks), len(self.triggers))

    def _result_since(self, mark: Sequence[int], order_id: int) -> OrderResult:
        fills, cancels, acks, triggers = mark
        new_acks = self.acks[acks:]

        status, message = None, ""
        for ack in new_acks:
            if ack.order_id == order_id:
                status, message = ack.status, ack.message
                break

        return OrderResult(
            order_id=order_id,
            status=status,
            message=message,
            fills=tuple(self.fills[fills:]),
            cancels=tuple(self.cancels[cancels:]),
            triggers=tuple(self.triggers[triggers:]),
        )

    @staticmethod
    def _dollars(basis_points: Optional[int]) -> Optional[float]:
        if basis_points is None:
            return None
        return _ob.from_basis_points(basis_points)
