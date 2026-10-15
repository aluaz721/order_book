from typing import Literal, Optional

from pydantic import BaseModel, Field


# ── Inbound requests ──────────────────────────────────────────────────────────

class OrderRequest(BaseModel):
    side: Literal["buy", "sell"]
    price: float = Field(..., gt=0)
    quantity: int = Field(..., gt=0)
    order_type: Literal["limit", "market", "market_limit",
                        "stop", "stop_limit", "ioc", "fok"] = "limit"
    symbol: str = "AAPL"

    # Stop types only.
    #
    # `stop_price` is the trigger — the traded price that turns the order
    # live. It falls back to `price` when omitted, so a stop can be posted
    # with the same body shape as a limit order.
    #
    # `limit_price` is required for `stop_limit`: the price the LIMIT order it
    # converts into carries. The engine's convention (order_validators.hpp) is
    # `limit_price` <= `stop_price` for buys and >= for sells, which keeps the
    # converted order passive; a body that breaks it comes back rejected with
    # the engine's own explanation.
    stop_price: Optional[float] = Field(None, gt=0)
    limit_price: Optional[float] = Field(None, gt=0)


class CancelRequest(BaseModel):
    order_id: int


class BacktestRequest(BaseModel):
    symbol: str = "AAPL"
    start_time: str = "2024-01-01T00:00:00Z"
    end_time: str = "2024-01-02T00:00:00Z"
    depth: int = Field(10, ge=1, le=50)


# ── Outbound responses ────────────────────────────────────────────────────────

class BookLevel(BaseModel):
    price: float
    quantity: int
    order_count: int
    order_ids: list[int]          # so the UI can show which orders sit here
    created_at_by_id: dict[int, str] = Field(default_factory=dict)


class RestingOrder(BaseModel):
    order_id: int
    side: Literal["buy", "sell"]
    price: float
    quantity: int
    orig_quantity: int
    order_type: str
    symbol: str
    created_at: str


class PendingStop(BaseModel):
    """A STOP or STOP_LIMIT order accepted but not yet triggered.

    Deliberately not a `RestingOrder`: a pending stop is held by the
    StopOrderManager, outside the book, so it has no price level, no queue
    position and no effect on depth or the touch. The UI lists these
    separately for exactly that reason — otherwise an accepted stop would
    look like an order that vanished.
    """

    order_id: int
    side: Literal["buy", "sell"]
    order_type: Literal["stop", "stop_limit"]
    stop_price: float
    limit_price: Optional[float]
    quantity: int
    symbol: str
    created_at: str


class FillRecord(BaseModel):
    sequence: int
    aggressive_order_id: int
    passive_order_id: int
    side: Literal["buy", "sell"]   # aggressor side
    price: float
    quantity: int
    symbol: str


class BookSnapshot(BaseModel):
    symbol: str
    bids: list[BookLevel]
    asks: list[BookLevel]
    best_bid: Optional[float]
    best_ask: Optional[float]
    last_trade_price: Optional[float]
    sequence: int
    resting_orders: list[RestingOrder]   # full flat list — UI renders the panels
    pending_stops: list[PendingStop] = Field(default_factory=list)


class OrderSubmissionResponse(BaseModel):
    accepted: bool
    order_id: int
    status: str
    message: str


class CancelResponse(BaseModel):
    accepted: bool
    order_id: int
    message: str


class BacktestRun(BaseModel):
    run_id: str
    status: str
    symbol: str
    start_time: str
    end_time: str
    depth: int
    message: str


class ReplayRequest(BaseModel):
    """Start a historical replay.

    `path` is a file name inside the server's data directory, not a full
    path — the service resolves it against that root and refuses anything
    that escapes.
    """

    path: str = "synthetic_aapl.itch"
    symbol: str = "AAPL"
    speed: Literal["fast", "realtime", "fixed"] = "fixed"
    # Gap between events for speed="fixed". 40ms reads as a lively but
    # followable book; "realtime" ignores this and uses the file's own gaps.
    fixed_ns: int = 40_000_000


class ReplayStatus(BaseModel):
    state: Literal["starting", "running", "finished", "stopped", "error"]
    path: str
    symbol: str
    speed: str
    fixed_ns: int
    events_replayed: int
    events_total: int
    fills: int
    elapsed_seconds: float
    error: Optional[str] = None
